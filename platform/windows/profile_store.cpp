#include "pseries/platform/profile_store.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <string>
namespace pseries::platform {
namespace {
Error win32_error(const char* operation, DWORD error) {
return Error{
ErrorCode::service_error,
std::string(operation) + " failed with Win32 error " +
std::to_string(error)};
}
Result<void> ensure_parent_directory(const std::wstring& path) {
const auto separator = path.find_last_of(L"\\/");
if (separator == std::wstring::npos || separator == 0) {
return Result<void>{{Error{
ErrorCode::invalid_argument,
"Profile path has no parent directory"}}};
}
const auto parent = path.substr(0, separator);
if (CreateDirectoryW(parent.c_str(), nullptr)) {
return Result<void>{};
}
const auto error = GetLastError();
if (error == ERROR_ALREADY_EXISTS) {
return Result<void>{};
}
return Result<void>{{win32_error("Create profile directory", error)}};
}
}
std::wstring p104_profile_path() {
wchar_t expanded[MAX_PATH]{};
const auto length = ExpandEnvironmentStringsW(
L"%ProgramData%\\P2UNLOCK\\profile.txt", expanded, MAX_PATH);
if (length == 0 || length > MAX_PATH) {
return L"C:\\ProgramData\\P2UNLOCK\\profile.txt";
}
return std::wstring(expanded, length - 1);
}
std::wstring default_driver_path() {
return std::wstring(kDefaultDriverPath);
}
Result<P104ServiceProfile> load_p104_service_profile() {
const auto path = p104_profile_path();
const auto handle = CreateFileW(
path.c_str(),
GENERIC_READ,
FILE_SHARE_READ,
nullptr,
OPEN_EXISTING,
FILE_ATTRIBUTE_NORMAL,
nullptr);
if (handle == INVALID_HANDLE_VALUE) {
const auto error = GetLastError();
if (error == ERROR_FILE_NOT_FOUND) {
return Result<P104ServiceProfile>{{Error{
ErrorCode::invalid_configuration,
"No unlock profile is saved"}}};
}
return Result<P104ServiceProfile>{{win32_error("Open profile", error)}};
}
const auto size = GetFileSize(handle, nullptr);
if (size == INVALID_FILE_SIZE || size > 64u * 1024u) {
CloseHandle(handle);
return Result<P104ServiceProfile>{{Error{
ErrorCode::invalid_configuration,
"Profile file is too large"}}};
}
std::string text(static_cast<std::size_t>(size), '\0');
DWORD read = 0;
const auto ok = size == 0 ||
ReadFile(handle, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
CloseHandle(handle);
if (!ok) {
return Result<P104ServiceProfile>{{win32_error("Read profile", GetLastError())}};
}
text.resize(read);
return parse_p104_service_profile(text);
}
Result<void> save_p104_service_profile(const P104ServiceProfile& profile) {
const auto serialized = serialize_p104_service_profile(profile);
if (!serialized.has_value()) {
return Result<void>{{serialized.error()}};
}
const auto path = p104_profile_path();
const auto directory = ensure_parent_directory(path);
if (!directory.has_value()) {
return directory;
}
const auto handle = CreateFileW(
path.c_str(),
GENERIC_WRITE,
0,
nullptr,
CREATE_ALWAYS,
FILE_ATTRIBUTE_NORMAL,
nullptr);
if (handle == INVALID_HANDLE_VALUE) {
return Result<void>{{win32_error("Create profile", GetLastError())}};
}
DWORD written = 0;
const auto ok = WriteFile(
handle,
serialized.get().data(),
static_cast<DWORD>(serialized.get().size()),
&written,
nullptr);
CloseHandle(handle);
if (!ok || written != serialized.get().size()) {
return Result<void>{{win32_error("Write profile", GetLastError())}};
}
return Result<void>{};
}
Result<void> clear_p104_service_profile() {
if (!DeleteFileW(p104_profile_path().c_str())) {
const auto error = GetLastError();
if (error == ERROR_FILE_NOT_FOUND) {
return Result<void>{};
}
return Result<void>{{win32_error("Remove profile", error)}};
}
return Result<void>{};
}
}
