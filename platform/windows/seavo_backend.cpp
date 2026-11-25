#include "pseries/platform/seavo_backend.hpp"
#include "pseries/seavo_protocol.hpp"
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
namespace pseries::platform {
namespace {
constexpr wchar_t seavo_device_path[] = L"\\\\.\\SvIoCtrl_1_0_0";
constexpr char expected_driver_hash[] =
"9ade632e54c3f5e8c75eaf636f2910bad3a8a07d6f078e48c9641d3af1fe811c";
std::atomic_uint64_t service_instance{0};
class ScopedFileHandle final {
public:
ScopedFileHandle() = default;
ScopedFileHandle(const ScopedFileHandle&) = delete;
ScopedFileHandle& operator=(const ScopedFileHandle&) = delete;
~ScopedFileHandle() {
if (handle_ != INVALID_HANDLE_VALUE) {
CloseHandle(handle_);
}
}
void reset(HANDLE handle) noexcept {
if (handle_ != INVALID_HANDLE_VALUE) {
CloseHandle(handle_);
}
handle_ = handle;
}
[[nodiscard]] HANDLE get() const noexcept {
return handle_;
}
private:
HANDLE handle_{INVALID_HANDLE_VALUE};
};
Error win32_error(const char* operation, DWORD error) {
return Error{
ErrorCode::service_error,
std::string(operation) + " failed with Win32 error " + std::to_string(error)};
}
Error ntstatus_error(const char* operation, NTSTATUS status) {
return Error{
ErrorCode::service_error,
std::string(operation) + " failed with NTSTATUS " +
std::to_string(static_cast<unsigned long>(status))};
}
Error validate_access(std::uint64_t address, std::uint32_t width) {
if (!seavo::is_supported_physical_width(width)) {
return Error{ErrorCode::invalid_argument, "Physical access width must be 1, 2, or 4 bytes"};
}
if (!seavo::is_aligned_physical_access(address, width)) {
return Error{ErrorCode::invalid_argument, "Physical access address is not naturally aligned"};
}
return Error{};
}
std::wstring resolve_driver_path(const std::wstring& path, Error& error) {
std::wstring result(512, L'\0');
DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(result.size()), result.data(), nullptr);
if (length == 0) {
error = win32_error("Resolve driver path", GetLastError());
return {};
}
if (length >= result.size()) {
result.resize(static_cast<std::size_t>(length) + 1);
length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(result.size()), result.data(), nullptr);
if (length == 0 || length >= result.size()) {
error = win32_error("Resolve driver path", GetLastError());
return {};
}
}
result.resize(length);
return result;
}
Result<void> verify_driver_hash(const std::wstring& path, ScopedFileHandle& file) {
file.reset(CreateFileW(
path.c_str(),
GENERIC_READ,
FILE_SHARE_READ,
nullptr,
OPEN_EXISTING,
FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
nullptr));
if (file.get() == INVALID_HANDLE_VALUE) {
return Result<void>{{win32_error("Open Seavo driver file", GetLastError())}};
}
BCRYPT_ALG_HANDLE algorithm = nullptr;
BCRYPT_HASH_HANDLE hash = nullptr;
std::vector<UCHAR> hash_object;
std::array<UCHAR, 32> digest{};
Error failure{};
DWORD object_length = 0;
DWORD digest_length = 0;
DWORD property_bytes = 0;
auto status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
if (status < 0) {
failure = ntstatus_error("Open SHA-256 provider", status);
}
if (failure.code == ErrorCode::none) {
status = BCryptGetProperty(
algorithm,
BCRYPT_OBJECT_LENGTH,
reinterpret_cast<PUCHAR>(&object_length),
sizeof(object_length),
&property_bytes,
0);
if (status < 0) {
failure = ntstatus_error("Read SHA-256 object size", status);
}
}
if (failure.code == ErrorCode::none) {
status = BCryptGetProperty(
algorithm,
BCRYPT_HASH_LENGTH,
reinterpret_cast<PUCHAR>(&digest_length),
sizeof(digest_length),
&property_bytes,
0);
if (status < 0 || digest_length != digest.size()) {
failure = status < 0
? ntstatus_error("Read SHA-256 digest size", status)
: Error{ErrorCode::service_error, "Unexpected SHA-256 digest size"};
}
}
if (failure.code == ErrorCode::none) {
hash_object.resize(object_length);
status = BCryptCreateHash(
algorithm,
&hash,
hash_object.data(),
object_length,
nullptr,
0,
0);
if (status < 0) {
failure = ntstatus_error("Create SHA-256 hash", status);
}
}
if (failure.code == ErrorCode::none) {
std::array<UCHAR, 64 * 1024> buffer{};
for (;;) {
DWORD bytes_read = 0;
if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr)) {
failure = win32_error("Read Seavo driver file", GetLastError());
break;
}
if (bytes_read == 0) {
break;
}
status = BCryptHashData(hash, buffer.data(), bytes_read, 0);
if (status < 0) {
failure = ntstatus_error("Hash Seavo driver file", status);
break;
}
}
}
if (failure.code == ErrorCode::none) {
status = BCryptFinishHash(hash, digest.data(), digest_length, 0);
if (status < 0) {
failure = ntstatus_error("Finish Seavo driver hash", status);
}
}
if (hash != nullptr) {
BCryptDestroyHash(hash);
}
if (algorithm != nullptr) {
BCryptCloseAlgorithmProvider(algorithm, 0);
}
if (failure.code != ErrorCode::none) {
return Result<void>{{failure}};
}
constexpr char digits[] = "0123456789abcdef";
std::string actual_hash;
actual_hash.reserve(digest.size() * 2);
for (const auto byte : digest) {
actual_hash.push_back(digits[byte >> 4]);
actual_hash.push_back(digits[byte & 0x0F]);
}
if (actual_hash != expected_driver_hash) {
return Result<void>{{Error{
ErrorCode::unsupported,
"Seavo driver SHA-256 does not match the reviewed external-download binary"}}};
}
return Result<void>{};
}
void append_cleanup_error(std::string& message, const char* operation, DWORD error) {
if (!message.empty()) {
message += "; ";
}
message += operation;
message += " failed with Win32 error ";
message += std::to_string(error);
}
std::string stop_and_delete_service(SC_HANDLE service) {
std::string errors;
SERVICE_STATUS_PROCESS status{};
DWORD status_bytes = 0;
bool stopped = false;
if (QueryServiceStatusEx(
service,
SC_STATUS_PROCESS_INFO,
reinterpret_cast<LPBYTE>(&status),
sizeof(status),
&status_bytes)) {
stopped = status.dwCurrentState == SERVICE_STOPPED;
} else {
append_cleanup_error(errors, "Query Seavo service before stop", GetLastError());
}
if (!stopped) {
SERVICE_STATUS control_status{};
if (!ControlService(service, SERVICE_CONTROL_STOP, &control_status)) {
const auto error = GetLastError();
if (error != ERROR_SERVICE_NOT_ACTIVE &&
error != ERROR_SERVICE_CANNOT_ACCEPT_CTRL) {
append_cleanup_error(errors, "Stop Seavo service", error);
}
}
const auto deadline = GetTickCount64() + 15000;
for (;;) {
if (!QueryServiceStatusEx(
service,
SC_STATUS_PROCESS_INFO,
reinterpret_cast<LPBYTE>(&status),
sizeof(status),
&status_bytes)) {
append_cleanup_error(errors, "Query Seavo service stop state", GetLastError());
break;
}
if (status.dwCurrentState == SERVICE_STOPPED) {
break;
}
if (GetTickCount64() >= deadline) {
append_cleanup_error(errors, "Wait for Seavo service stop", ERROR_TIMEOUT);
break;
}
Sleep(100);
}
}
if (!DeleteService(service)) {
append_cleanup_error(errors, "Delete Seavo service", GetLastError());
}
return errors;
}
}
SeavoBackend::SeavoBackend(HANDLE device, SC_HANDLE manager, SC_HANDLE service) noexcept
: device_(device),
manager_(manager),
service_(service) {
}
SeavoBackend::~SeavoBackend() {
static_cast<void>(close());
}
Result<std::unique_ptr<SeavoBackend>> SeavoBackend::load(const std::wstring& driver_path) {
Error path_error{};
const auto resolved_driver_path = resolve_driver_path(driver_path, path_error);
if (path_error.code != ErrorCode::none) {
return Result<std::unique_ptr<SeavoBackend>>{{path_error}};
}
ScopedFileHandle verified_driver_file;
const auto hash_result = verify_driver_hash(resolved_driver_path, verified_driver_file);
if (!hash_result.has_value()) {
return Result<std::unique_ptr<SeavoBackend>>{{hash_result.error()}};
}
const auto existing_device = CreateFileW(
seavo_device_path,
GENERIC_READ | GENERIC_WRITE,
FILE_SHARE_READ | FILE_SHARE_WRITE,
nullptr,
OPEN_EXISTING,
FILE_ATTRIBUTE_NORMAL,
nullptr);
if (existing_device != INVALID_HANDLE_VALUE) {
CloseHandle(existing_device);
return Result<std::unique_ptr<SeavoBackend>>{{Error{
ErrorCode::unsupported,
"Seavo device is already present; refusing to take over an existing driver"}}};
}
const auto existing_device_error = GetLastError();
if (existing_device_error != ERROR_FILE_NOT_FOUND &&
existing_device_error != ERROR_PATH_NOT_FOUND) {
return Result<std::unique_ptr<SeavoBackend>>{{win32_error(
"Check existing Seavo device", existing_device_error)}};
}
const auto manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
if (manager == nullptr) {
return Result<std::unique_ptr<SeavoBackend>>{{win32_error("Open Service Control Manager", GetLastError())}};
}
const auto service_name = L"PSeriesNativeSeavo_" +
std::to_wstring(GetCurrentProcessId()) + L"_" +
std::to_wstring(service_instance.fetch_add(1));
const auto service_image_path = L"\\??\\" + resolved_driver_path;
const auto service = CreateServiceW(
manager,
service_name.c_str(),
service_name.c_str(),
SERVICE_START | SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS,
SERVICE_KERNEL_DRIVER,
SERVICE_DEMAND_START,
SERVICE_ERROR_NORMAL,
service_image_path.c_str(),
nullptr,
nullptr,
nullptr,
nullptr,
nullptr);
if (service == nullptr) {
const auto error = GetLastError();
CloseServiceHandle(manager);
return Result<std::unique_ptr<SeavoBackend>>{{win32_error("Create temporary Seavo service", error)}};
}
if (!StartServiceW(service, 0, nullptr)) {
const auto error = GetLastError();
std::string message = win32_error("Start Seavo driver service", error).message;
if (!DeleteService(service)) {
append_cleanup_error(message, "Delete failed driver service", GetLastError());
}
CloseServiceHandle(service);
CloseServiceHandle(manager);
return Result<std::unique_ptr<SeavoBackend>>{{Error{ErrorCode::service_error, message}}};
}
SERVICE_STATUS_PROCESS status{};
DWORD status_bytes = 0;
const auto deadline = GetTickCount64() + 15000;
for (;;) {
if (!QueryServiceStatusEx(
service,
SC_STATUS_PROCESS_INFO,
reinterpret_cast<LPBYTE>(&status),
sizeof(status),
&status_bytes)) {
const auto error = GetLastError();
std::string message = win32_error("Query Seavo service state", error).message;
const auto cleanup = stop_and_delete_service(service);
if (!cleanup.empty()) {
message += "; cleanup: ";
message += cleanup;
}
CloseServiceHandle(service);
CloseServiceHandle(manager);
return Result<std::unique_ptr<SeavoBackend>>{{Error{ErrorCode::service_error, message}}};
}
if (status.dwCurrentState == SERVICE_RUNNING) {
break;
}
if (status.dwCurrentState != SERVICE_START_PENDING || GetTickCount64() >= deadline) {
std::string message = "Seavo service did not reach RUNNING; Win32 exit " +
std::to_string(status.dwWin32ExitCode);
const auto cleanup = stop_and_delete_service(service);
if (!cleanup.empty()) {
message += "; cleanup: ";
message += cleanup;
}
CloseServiceHandle(service);
CloseServiceHandle(manager);
return Result<std::unique_ptr<SeavoBackend>>{{Error{ErrorCode::service_error, message}}};
}
Sleep(100);
}
const auto device = CreateFileW(
seavo_device_path,
GENERIC_READ | GENERIC_WRITE,
FILE_SHARE_READ | FILE_SHARE_WRITE,
nullptr,
OPEN_EXISTING,
FILE_ATTRIBUTE_NORMAL,
nullptr);
if (device == INVALID_HANDLE_VALUE) {
const auto error = GetLastError();
auto backend = std::unique_ptr<SeavoBackend>(
new SeavoBackend(INVALID_HANDLE_VALUE, manager, service));
std::string message = win32_error("Open Seavo device after driver start", error).message;
const auto cleanup = backend->close();
if (!cleanup.has_value()) {
message += "; cleanup: ";
message += cleanup.error().message;
}
return Result<std::unique_ptr<SeavoBackend>>{{Error{ErrorCode::service_error, message}}};
}
return Result<std::unique_ptr<SeavoBackend>>{
std::unique_ptr<SeavoBackend>(new SeavoBackend(device, manager, service))};
}
Result<void> SeavoBackend::close() {
std::string cleanup_errors;
if (device_ != INVALID_HANDLE_VALUE) {
if (!CloseHandle(device_)) {
append_cleanup_error(cleanup_errors, "Close Seavo device", GetLastError());
}
device_ = INVALID_HANDLE_VALUE;
}
if (service_ != nullptr) {
const auto service_errors = stop_and_delete_service(service_);
if (!service_errors.empty()) {
if (!cleanup_errors.empty()) {
cleanup_errors += "; ";
}
cleanup_errors += service_errors;
}
if (!CloseServiceHandle(service_)) {
append_cleanup_error(cleanup_errors, "Close Seavo service handle", GetLastError());
}
service_ = nullptr;
}
if (manager_ != nullptr) {
if (!CloseServiceHandle(manager_)) {
append_cleanup_error(cleanup_errors, "Close service manager handle", GetLastError());
}
manager_ = nullptr;
}
if (!cleanup_errors.empty()) {
return Result<void>{{Error{ErrorCode::service_error, cleanup_errors}}};
}
return Result<void>{};
}
Result<std::uint32_t> SeavoBackend::read_physical(
std::uint64_t address,
std::uint32_t width) const {
const auto validation = validate_access(address, width);
if (validation.code != ErrorCode::none) {
return Result<std::uint32_t>{{validation}};
}
seavo::PhysicalAccessHeader request{address, width, 1};
std::array<std::byte, sizeof(std::uint32_t)> output{};
DWORD bytes_returned = 0;
if (!DeviceIoControl(
device_,
seavo::physical_read_ioctl,
&request,
sizeof(request),
output.data(),
width,
&bytes_returned,
nullptr)) {
return Result<std::uint32_t>{{win32_error("Read physical memory", GetLastError())}};
}
if (bytes_returned != width) {
return Result<std::uint32_t>{{Error{
ErrorCode::service_error,
"Seavo physical read returned an unexpected byte count"}}};
}
std::uint32_t value = 0;
std::memcpy(&value, output.data(), width);
return Result<std::uint32_t>{value};
}
Result<void> SeavoBackend::write_physical(
std::uint64_t address,
std::uint32_t width,
std::uint32_t value) const {
const auto validation = validate_access(address, width);
if (validation.code != ErrorCode::none) {
return Result<void>{{validation}};
}
std::array<std::byte, seavo::physical_access_header_size + sizeof(value)> input{};
const seavo::PhysicalAccessHeader request{address, width, 1};
std::memcpy(input.data(), &request, sizeof(request));
std::memcpy(input.data() + sizeof(request), &value, width);
DWORD bytes_returned = 0;
if (!DeviceIoControl(
device_,
seavo::physical_write_ioctl,
input.data(),
static_cast<DWORD>(sizeof(request) + width),
nullptr,
0,
&bytes_returned,
nullptr)) {
return Result<void>{{win32_error("Write physical memory", GetLastError())}};
}
if (bytes_returned != 0) {
return Result<void>{{Error{
ErrorCode::service_error,
"Seavo physical write returned an unexpected byte count"}}};
}
return Result<void>{};
}
}
