#include "pseries/platform/service_ipc.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
namespace pseries::platform {
namespace {
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\P2UNLOCK";
constexpr DWORD kPipeBufferSize = 4096;
constexpr std::size_t kBufferSize = 4096;
constexpr DWORD kStopPollMs = 50;
#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x00000008
#endif
constexpr DWORD kPipeAceCapacity = static_cast<DWORD>(
sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + SECURITY_MAX_SID_SIZE);
constexpr DWORD kPipeAclCapacity = static_cast<DWORD>(sizeof(ACL)) +
2 * kPipeAceCapacity;
struct PipeSecurity {
SECURITY_DESCRIPTOR descriptor{};
alignas(ACL) BYTE acl_storage[kPipeAclCapacity]{};
BYTE system_sid[SECURITY_MAX_SID_SIZE]{};
BYTE administrators_sid[SECURITY_MAX_SID_SIZE]{};
};
static_assert(kPipeAclCapacity >= static_cast<DWORD>(sizeof(ACL)));
static_assert(sizeof(PipeSecurity::acl_storage) ==
static_cast<std::size_t>(kPipeAclCapacity));
static_assert(alignof(PipeSecurity) >= alignof(ACL));
ACL* pipe_acl(PipeSecurity& security) noexcept {
return reinterpret_cast<ACL*>(security.acl_storage);
}
class ScopedHandle final {
public:
explicit ScopedHandle(
HANDLE value = INVALID_HANDLE_VALUE,
HANDLE invalid_value = INVALID_HANDLE_VALUE) noexcept
: value_(value), invalid_value_(invalid_value) {
}
ScopedHandle(const ScopedHandle&) = delete;
ScopedHandle& operator=(const ScopedHandle&) = delete;
~ScopedHandle() {
if (value_ != invalid_value_) {
CloseHandle(value_);
}
}
[[nodiscard]] bool valid() const noexcept {
return value_ != invalid_value_;
}
[[nodiscard]] HANDLE get() const noexcept {
return value_;
}
private:
HANDLE value_;
HANDLE invalid_value_;
};
enum class IoResult {
completed,
stopped,
disconnected,
failed,
};
bool is_client_disconnect(DWORD error) noexcept {
return error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA ||
error == ERROR_PIPE_NOT_CONNECTED;
}
std::string to_utf8(std::wstring_view value) {
if (value.empty()) {
return {};
}
const int length = WideCharToMultiByte(
CP_UTF8,
0,
value.data(),
static_cast<int>(value.size()),
nullptr,
0,
nullptr,
nullptr);
if (length <= 0) {
return {};
}
std::string result(static_cast<std::size_t>(length), '\0');
const int written = WideCharToMultiByte(
CP_UTF8,
0,
value.data(),
static_cast<int>(value.size()),
result.data(),
length,
nullptr,
nullptr);
if (written <= 0) {
return {};
}
result.resize(static_cast<std::size_t>(written));
return result;
}
std::string win32_error_text(DWORD error) {
std::wstring buffer(512, L'\0');
const DWORD length = FormatMessageW(
FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
nullptr,
error,
MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
buffer.data(),
static_cast<DWORD>(buffer.size()),
nullptr);
if (length == 0) {
return "Win32 error " + std::to_string(error);
}
std::size_t text_length = static_cast<std::size_t>(length);
while (text_length > 0 &&
(buffer[text_length - 1] == L'\r' || buffer[text_length - 1] == L'\n')) {
--text_length;
}
std::string text = to_utf8(std::wstring_view(buffer.data(), text_length));
if (text.empty()) {
return "Win32 error " + std::to_string(error);
}
return text;
}
Error service_error(const char* operation, DWORD error) {
return Error{
ErrorCode::service_error,
std::string(operation) + " failed: " + win32_error_text(error) +
" (" + std::to_string(error) + ")"};
}
Result<void> initialize_pipe_security(PipeSecurity& security) {
DWORD system_sid_size = static_cast<DWORD>(sizeof(security.system_sid));
if (!CreateWellKnownSid(
WinLocalSystemSid, nullptr, security.system_sid, &system_sid_size)) {
return Result<void>{{service_error(
"Create local system SID", GetLastError())}};
}
DWORD administrators_sid_size =
static_cast<DWORD>(sizeof(security.administrators_sid));
if (!CreateWellKnownSid(
WinBuiltinAdministratorsSid, nullptr,
security.administrators_sid, &administrators_sid_size)) {
return Result<void>{{service_error(
"Create administrators SID", GetLastError())}};
}
const DWORD acl_size = static_cast<DWORD>(sizeof(security.acl_storage));
if (!InitializeAcl(pipe_acl(security), acl_size, ACL_REVISION)) {
return Result<void>{{service_error("Initialize pipe ACL", GetLastError())}};
}
if (!AddAccessAllowedAce(
pipe_acl(security), ACL_REVISION, GENERIC_ALL, security.system_sid)) {
return Result<void>{{service_error(
"Allow SYSTEM pipe access", GetLastError())}};
}
if (!AddAccessAllowedAce(
pipe_acl(security), ACL_REVISION, GENERIC_ALL,
security.administrators_sid)) {
return Result<void>{{service_error(
"Allow Administrators pipe access", GetLastError())}};
}
if (!InitializeSecurityDescriptor(
&security.descriptor, SECURITY_DESCRIPTOR_REVISION)) {
return Result<void>{{service_error(
"Initialize pipe security descriptor", GetLastError())}};
}
if (!SetSecurityDescriptorDacl(
&security.descriptor, TRUE, pipe_acl(security), FALSE)) {
return Result<void>{{service_error("Set pipe DACL", GetLastError())}};
}
return Result<void>{};
}
class StopMonitor final {
public:
StopMonitor(const std::atomic_bool& stop, HANDLE event) noexcept
: stop_(stop), event_(event) {
}
StopMonitor(const StopMonitor&) = delete;
StopMonitor& operator=(const StopMonitor&) = delete;
~StopMonitor() {
SetEvent(event_);
if (thread_.joinable()) {
thread_.join();
}
}
void start() {
if (stop_.load()) {
SetEvent(event_);
return;
}
thread_ = std::thread([this] {
while (!stop_.load()) {
Sleep(kStopPollMs);
}
SetEvent(event_);
});
}
private:
const std::atomic_bool& stop_;
HANDLE event_;
std::thread thread_;
};
void cancel_overlapped(HANDLE file, OVERLAPPED& overlapped) {
CancelIoEx(file, &overlapped);
DWORD transferred = 0;
GetOverlappedResult(file, &overlapped, &transferred, TRUE);
}
IoResult wait_overlapped(
HANDLE file,
OVERLAPPED& overlapped,
HANDLE stop_event,
DWORD& transferred,
Error& error) {
HANDLE events[] = {overlapped.hEvent, stop_event};
const DWORD result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
if (result == WAIT_OBJECT_0) {
transferred = 0;
if (GetOverlappedResult(file, &overlapped, &transferred, FALSE)) {
return IoResult::completed;
}
const DWORD code = GetLastError();
if (is_client_disconnect(code)) {
return IoResult::disconnected;
}
if (code == ERROR_OPERATION_ABORTED &&
WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {
return IoResult::stopped;
}
error = service_error("Complete service pipe I/O", code);
return IoResult::failed;
}
if (result == WAIT_OBJECT_0 + 1) {
cancel_overlapped(file, overlapped);
return IoResult::stopped;
}
const DWORD code = result == WAIT_FAILED ? GetLastError() : result;
error = service_error("Wait for service pipe I/O", code);
return IoResult::failed;
}
IoResult read_command_line(
HANDLE pipe,
HANDLE stop_event,
std::string& command,
bool& command_ready,
Error& error) {
command.clear();
command_ready = false;
char buffer[kBufferSize];
for (;;) {
if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {
return IoResult::stopped;
}
OVERLAPPED overlapped{};
const ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr), nullptr);
if (!event.valid()) {
error = service_error("Create service read event", GetLastError());
return IoResult::failed;
}
overlapped.hEvent = event.get();
DWORD read = 0;
if (!ReadFile(
pipe,
buffer,
static_cast<DWORD>(sizeof(buffer)),
&read,
&overlapped)) {
const DWORD code = GetLastError();
if (code != ERROR_IO_PENDING) {
if (is_client_disconnect(code)) {
command_ready = !command.empty();
return IoResult::disconnected;
}
error = service_error("Read service command", code);
return IoResult::failed;
}
DWORD transferred = 0;
const auto result = wait_overlapped(
pipe, overlapped, stop_event, transferred, error);
if (result == IoResult::completed) {
read = transferred;
} else {
if (result == IoResult::disconnected) {
command_ready = !command.empty();
}
return result;
}
}
if (read == 0) {
command_ready = !command.empty();
return IoResult::disconnected;
}
const auto end = buffer + static_cast<std::size_t>(read);
const auto newline = std::find(buffer, end, '\n');
const auto count = static_cast<std::size_t>(newline - buffer);
command.append(buffer, count);
if (newline != end) {
if (!command.empty() && command.back() == '\r') {
command.pop_back();
}
command_ready = true;
return IoResult::completed;
}
}
}
IoResult write_all(
HANDLE pipe,
HANDLE stop_event,
const std::string& data,
Error& error) {
std::size_t offset = 0;
while (offset < data.size()) {
if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {
return IoResult::stopped;
}
OVERLAPPED overlapped{};
const ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr), nullptr);
if (!event.valid()) {
error = service_error("Create service write event", GetLastError());
return IoResult::failed;
}
overlapped.hEvent = event.get();
const DWORD chunk = static_cast<DWORD>(
std::min<std::size_t>(data.size() - offset, kBufferSize));
DWORD written = 0;
if (!WriteFile(
pipe,
data.data() + offset,
chunk,
&written,
&overlapped)) {
const DWORD code = GetLastError();
if (code != ERROR_IO_PENDING) {
if (is_client_disconnect(code)) {
return IoResult::disconnected;
}
error = service_error("Write service response", code);
return IoResult::failed;
}
DWORD transferred = 0;
const auto result = wait_overlapped(
pipe, overlapped, stop_event, transferred, error);
if (result == IoResult::completed) {
written = transferred;
} else {
return result;
}
}
if (written == 0) {
error = Error{
ErrorCode::service_error,
"Write service response returned zero bytes"};
return IoResult::failed;
}
offset += written;
}
return IoResult::completed;
}
}
Result<void> send_service_command(
std::string_view command,
std::string& response) {
response.clear();
const ScopedHandle pipe(CreateFileW(
kPipeName,
GENERIC_READ | GENERIC_WRITE,
0,
nullptr,
OPEN_EXISTING,
FILE_ATTRIBUTE_NORMAL,
nullptr));
if (!pipe.valid()) {
return Result<void>{{service_error(
"Open P2UNLOCK service pipe", GetLastError())}};
}
std::string payload(command);
payload.push_back('\n');
std::size_t offset = 0;
while (offset < payload.size()) {
const DWORD chunk = static_cast<DWORD>(
std::min<std::size_t>(payload.size() - offset, kBufferSize));
DWORD written = 0;
if (!WriteFile(
pipe.get(),
payload.data() + offset,
chunk,
&written,
nullptr)) {
return Result<void>{{service_error("Write service command", GetLastError())}};
}
if (written == 0) {
return Result<void>{{Error{
ErrorCode::service_error,
"Write service command returned zero bytes"}}};
}
offset += written;
}
char buffer[kBufferSize];
for (;;) {
DWORD read = 0;
if (!ReadFile(
pipe.get(),
buffer,
static_cast<DWORD>(sizeof(buffer)),
&read,
nullptr)) {
const DWORD code = GetLastError();
if (is_client_disconnect(code)) {
break;
}
return Result<void>{{service_error("Read service response", code)}};
}
if (read == 0) {
break;
}
response.append(buffer, static_cast<std::size_t>(read));
}
return Result<void>{};
}
Result<void> serve_service_commands(
const std::atomic_bool& stop,
const std::function<std::string(std::string)>& handler) {
const ScopedHandle stop_event(CreateEventW(nullptr, TRUE, FALSE, nullptr), nullptr);
if (!stop_event.valid()) {
return Result<void>{{service_error(
"Create service stop event", GetLastError())}};
}
StopMonitor monitor(stop, stop_event.get());
try {
monitor.start();
} catch (...) {
return Result<void>{{Error{
ErrorCode::service_error,
"Failed to start service stop monitor"}}};
}
PipeSecurity pipe_security{};
const auto secured = initialize_pipe_security(pipe_security);
if (!secured.has_value()) {
return Result<void>{{secured.error()}};
}
SECURITY_ATTRIBUTES pipe_attributes{};
pipe_attributes.nLength = sizeof(pipe_attributes);
pipe_attributes.lpSecurityDescriptor = &pipe_security.descriptor;
pipe_attributes.bInheritHandle = FALSE;
for (;;) {
if (WaitForSingleObject(stop_event.get(), 0) == WAIT_OBJECT_0) {
break;
}
const ScopedHandle pipe(CreateNamedPipeW(
kPipeName,
PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
PIPE_REJECT_REMOTE_CLIENTS,
PIPE_UNLIMITED_INSTANCES,
kPipeBufferSize,
kPipeBufferSize,
NMPWAIT_USE_DEFAULT_WAIT,
&pipe_attributes));
if (!pipe.valid()) {
const DWORD code = GetLastError();
if (WaitForSingleObject(stop_event.get(), 0) == WAIT_OBJECT_0) {
break;
}
return Result<void>{{service_error("Create P2UNLOCK service pipe", code)}};
}
OVERLAPPED connect{};
const ScopedHandle connect_event(
CreateEventW(nullptr, TRUE, FALSE, nullptr),
nullptr);
if (!connect_event.valid()) {
return Result<void>{{service_error(
"Create service connection event", GetLastError())}};
}
connect.hEvent = connect_event.get();
BOOL connected = ConnectNamedPipe(pipe.get(), &connect);
DWORD connect_error = connected ? NO_ERROR : GetLastError();
if (!connected && connect_error == ERROR_IO_PENDING) {
DWORD transferred = 0;
Error connect_failure{};
const auto result = wait_overlapped(
pipe.get(),
connect,
stop_event.get(),
transferred,
connect_failure);
if (result == IoResult::stopped) {
break;
}
if (result == IoResult::failed) {
return Result<void>{{connect_failure}};
}
if (result == IoResult::disconnected) {
continue;
}
connected = TRUE;
} else if (!connected && connect_error == ERROR_PIPE_CONNECTED) {
connected = TRUE;
} else if (!connected) {
if (is_client_disconnect(connect_error)) {
continue;
}
return Result<void>{{service_error(
"Connect P2UNLOCK service pipe", connect_error)}};
}
std::string command;
bool command_ready = false;
Error read_failure{};
const auto read_result = read_command_line(
pipe.get(),
stop_event.get(),
command,
command_ready,
read_failure);
if (read_result == IoResult::stopped) {
break;
}
if (read_result == IoResult::failed) {
return Result<void>{{read_failure}};
}
if (!command_ready) {
continue;
}
std::string response_text;
try {
response_text = handler(command);
} catch (...) {
return Result<void>{{Error{
ErrorCode::service_error,
"P2UNLOCK service command handler threw an exception"}}};
}
Error write_failure{};
const auto write_result = write_all(
pipe.get(),
stop_event.get(),
response_text,
write_failure);
if (write_result == IoResult::stopped) {
break;
}
if (write_result == IoResult::failed) {
return Result<void>{{write_failure}};
}
if (!FlushFileBuffers(pipe.get())) {
const DWORD flush_error = GetLastError();
if (WaitForSingleObject(stop_event.get(), 0) == WAIT_OBJECT_0) {
break;
}
if (!is_client_disconnect(flush_error)) {
return Result<void>{{service_error(
"Flush P2UNLOCK service response", flush_error)}};
}
}
DisconnectNamedPipe(pipe.get());
}
return Result<void>{};
}
}
