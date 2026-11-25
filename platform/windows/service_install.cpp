#include "pseries/platform/service_install.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winsvc.h>
#include <cstddef>
#include <string>
#include <string_view>
namespace pseries::platform {
namespace {
class ScopedServiceHandle final {
public:
explicit ScopedServiceHandle(SC_HANDLE value = nullptr) noexcept : value_(value) {
}
ScopedServiceHandle(const ScopedServiceHandle&) = delete;
ScopedServiceHandle& operator=(const ScopedServiceHandle&) = delete;
~ScopedServiceHandle() {
if (value_ != nullptr) {
CloseServiceHandle(value_);
}
}
[[nodiscard]] bool valid() const noexcept {
return value_ != nullptr;
}
[[nodiscard]] SC_HANDLE get() const noexcept {
return value_;
}
private:
SC_HANDLE value_;
};
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
bool wait_for_service_state(SC_HANDLE service, DWORD target, DWORD timeout_ms) {
const auto deadline = GetTickCount64() + timeout_ms;
SERVICE_STATUS status{};
while (GetTickCount64() < deadline) {
if (!QueryServiceStatus(service, &status)) {
return false;
}
if (status.dwCurrentState == target) {
return true;
}
Sleep(100);
}
return false;
}
std::wstring quote_argument(const std::wstring& value) {
std::wstring quoted;
quoted.reserve(value.size() + 2);
quoted.push_back(L'"');
std::size_t backslashes = 0;
for (const wchar_t character : value) {
if (character == L'\\') {
++backslashes;
continue;
}
if (character == L'"') {
quoted.append(backslashes * 2 + 1, L'\\');
quoted.push_back(L'"');
} else {
quoted.append(backslashes, L'\\');
quoted.push_back(character);
}
backslashes = 0;
}
quoted.append(backslashes * 2, L'\\');
quoted.push_back(L'"');
return quoted;
}
void set_service_description(SC_HANDLE service, const std::wstring& driver_path) {
if (driver_path.empty()) {
return;
}
std::wstring description = driver_path;
SERVICE_DESCRIPTIONW value{};
value.lpDescription = description.data();
ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &value);
}
}
Result<void> install_p2unlock_service(
const std::wstring& exe_path,
const std::wstring& driver_path) {
if (exe_path.empty()) {
return Result<void>{{Error{
ErrorCode::invalid_argument,
"Service executable path must not be empty"}}};
}
const SC_HANDLE raw_manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
if (raw_manager == nullptr) {
return Result<void>{{service_error(
"Open service control manager", GetLastError())}};
}
const ScopedServiceHandle manager(raw_manager);
const std::wstring binary_path = quote_argument(exe_path) + L" --service";
const SC_HANDLE raw_service = CreateServiceW(
manager.get(),
kP2UnlockServiceName,
kP2UnlockServiceName,
SERVICE_ALL_ACCESS,
SERVICE_WIN32_OWN_PROCESS,
SERVICE_AUTO_START,
SERVICE_ERROR_NORMAL,
binary_path.c_str(),
nullptr,
nullptr,
nullptr,
nullptr,
nullptr);
if (raw_service != nullptr) {
const ScopedServiceHandle service(raw_service);
set_service_description(service.get(), driver_path);
return Result<void>{};
}
const DWORD create_error = GetLastError();
if (create_error != ERROR_SERVICE_EXISTS) {
return Result<void>{{service_error("Create P2UNLOCK service", create_error)}};
}
const SC_HANDLE raw_existing = OpenServiceW(
manager.get(),
kP2UnlockServiceName,
SERVICE_ALL_ACCESS);
if (raw_existing == nullptr) {
return Result<void>{{service_error(
"Open existing P2UNLOCK service", GetLastError())}};
}
const ScopedServiceHandle service(raw_existing);
if (!ChangeServiceConfigW(
service.get(),
SERVICE_NO_CHANGE,
SERVICE_AUTO_START,
SERVICE_NO_CHANGE,
binary_path.c_str(),
nullptr,
nullptr,
nullptr,
nullptr,
nullptr,
nullptr)) {
return Result<void>{{service_error(
"Configure existing P2UNLOCK service", GetLastError())}};
}
set_service_description(service.get(), driver_path);
return Result<void>{};
}
Result<void> uninstall_p2unlock_service() {
const SC_HANDLE raw_manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
if (raw_manager == nullptr) {
return Result<void>{{service_error(
"Open service control manager", GetLastError())}};
}
const ScopedServiceHandle manager(raw_manager);
const SC_HANDLE raw_service = OpenServiceW(
manager.get(),
kP2UnlockServiceName,
DELETE);
if (raw_service == nullptr) {
const DWORD error = GetLastError();
if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
return Result<void>{};
}
return Result<void>{{service_error(
"Open P2UNLOCK service for deletion", error)}};
}
const ScopedServiceHandle service(raw_service);
if (!DeleteService(service.get())) {
return Result<void>{{service_error("Delete P2UNLOCK service", GetLastError())}};
}
return Result<void>{};
}
Result<void> start_p2unlock_service() {
const SC_HANDLE raw_manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
if (raw_manager == nullptr) {
return Result<void>{{service_error(
"Open service control manager", GetLastError())}};
}
const ScopedServiceHandle manager(raw_manager);
const SC_HANDLE raw_service = OpenServiceW(
manager.get(),
kP2UnlockServiceName,
SERVICE_START | SERVICE_QUERY_STATUS);
if (raw_service == nullptr) {
const DWORD error = GetLastError();
if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
return Result<void>{{Error{
ErrorCode::service_error,
"P2UNLOCK service is not installed"}}};
}
return Result<void>{{service_error("Open P2UNLOCK service", error)}};
}
const ScopedServiceHandle service(raw_service);
if (!StartServiceW(service.get(), 0, nullptr)) {
const DWORD error = GetLastError();
if (error == ERROR_SERVICE_ALREADY_RUNNING) {
return Result<void>{};
}
return Result<void>{{service_error("Start P2UNLOCK service", error)}};
}
if (!wait_for_service_state(service.get(), SERVICE_RUNNING, 10000)) {
return Result<void>{{service_error(
"Wait for P2UNLOCK service start", ERROR_SERVICE_REQUEST_TIMEOUT)}};
}
return Result<void>{};
}
Result<void> stop_p2unlock_service() {
const SC_HANDLE raw_manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
if (raw_manager == nullptr) {
return Result<void>{{service_error(
"Open service control manager", GetLastError())}};
}
const ScopedServiceHandle manager(raw_manager);
const SC_HANDLE raw_service = OpenServiceW(
manager.get(), kP2UnlockServiceName,
SERVICE_STOP | SERVICE_QUERY_STATUS);
if (raw_service == nullptr) {
const DWORD error = GetLastError();
if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
return Result<void>{};
}
return Result<void>{{service_error("Open P2UNLOCK service", error)}};
}
const ScopedServiceHandle service(raw_service);
SERVICE_STATUS status{};
if (!ControlService(service.get(), SERVICE_CONTROL_STOP, &status)) {
const DWORD error = GetLastError();
if (error == ERROR_SERVICE_NOT_ACTIVE ||
error == ERROR_SERVICE_CANNOT_ACCEPT_CTRL) {
return Result<void>{};
}
return Result<void>{{service_error("Stop P2UNLOCK service", error)}};
}
if (!wait_for_service_state(service.get(), SERVICE_STOPPED, 10000)) {
return Result<void>{{service_error(
"Wait for P2UNLOCK service stop", ERROR_SERVICE_REQUEST_TIMEOUT)}};
}
return Result<void>{};
}
}
