#include "pseries/platform/transaction_journal_store.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
namespace pseries::platform {
namespace {
constexpr std::size_t maximum_journal_size = 512;
std::atomic_uint32_t temporary_file_sequence{0};
Error win32_error(const char* operation, DWORD error) {
return Error{
ErrorCode::service_error,
std::string(operation) + " failed with Win32 error " + std::to_string(error)};
}
void append_cleanup_error(std::string& message, const char* operation, DWORD error) {
message += "; ";
message += operation;
message += " failed with Win32 error ";
message += std::to_string(error);
}
std::wstring temporary_path_for(const std::wstring& path) {
return path + L".tmp." +
std::to_wstring(GetCurrentProcessId()) + L"." +
std::to_wstring(temporary_file_sequence.fetch_add(1));
}
void close_file(HANDLE& file, std::string& message) {
if (file == INVALID_HANDLE_VALUE) {
return;
}
const auto handle = file;
file = INVALID_HANDLE_VALUE;
if (!CloseHandle(handle)) {
const auto error = GetLastError();
if (message.empty()) {
message = win32_error("Close transaction journal", error).message;
} else {
append_cleanup_error(message, "Close transaction journal", error);
}
}
}
}
Result<void> save_transaction_journal(
const std::wstring& path,
const P104TransactionJournal& journal) {
if (path.empty()) {
return Result<void>{{Error{
ErrorCode::invalid_argument,
"Transaction journal path must not be empty"}}};
}
const auto encoded = serialize_transaction_journal(journal);
if (!encoded.has_value()) {
return Result<void>{{encoded.error()}};
}
if (encoded.get().size() > maximum_journal_size) {
return Result<void>{{Error{
ErrorCode::invalid_recovery_state,
"Encoded transaction journal exceeds the storage limit"}}};
}
const auto attributes = GetFileAttributesW(path.c_str());
if (attributes != INVALID_FILE_ATTRIBUTES &&
(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
return Result<void>{{Error{
ErrorCode::invalid_argument,
"Transaction journal destination must be a regular file"}}};
}
if (attributes == INVALID_FILE_ATTRIBUTES) {
const auto error = GetLastError();
if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
return Result<void>{{win32_error("Inspect transaction journal destination", error)}};
}
}
std::wstring temporary_path;
HANDLE file = INVALID_HANDLE_VALUE;
for (unsigned attempt = 0; attempt < 16; ++attempt) {
temporary_path = temporary_path_for(path);
file = CreateFileW(
temporary_path.c_str(),
GENERIC_WRITE,
0,
nullptr,
CREATE_NEW,
FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
nullptr);
if (file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) {
break;
}
}
if (file == INVALID_HANDLE_VALUE) {
return Result<void>{{win32_error("Create temporary transaction journal", GetLastError())}};
}
std::string failure;
std::size_t written_total = 0;
while (written_total < encoded.get().size()) {
const auto remaining = encoded.get().size() - written_total;
const auto chunk = static_cast<DWORD>(remaining);
DWORD written = 0;
if (!WriteFile(
file,
encoded.get().data() + written_total,
chunk,
&written,
nullptr)) {
failure = win32_error("Write transaction journal", GetLastError()).message;
break;
}
if (written == 0) {
failure = "Write transaction journal returned zero bytes";
break;
}
written_total += written;
}
if (failure.empty() && !FlushFileBuffers(file)) {
failure = win32_error("Flush transaction journal", GetLastError()).message;
}
close_file(file, failure);
if (!failure.empty()) {
if (!DeleteFileW(temporary_path.c_str()) &&
GetLastError() != ERROR_FILE_NOT_FOUND) {
append_cleanup_error(failure, "Delete temporary transaction journal", GetLastError());
}
return Result<void>{{Error{ErrorCode::service_error, failure}}};
}
if (!MoveFileExW(
temporary_path.c_str(),
path.c_str(),
MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
const auto error = GetLastError();
std::string message = win32_error("Replace transaction journal", error).message;
if (!DeleteFileW(temporary_path.c_str()) &&
GetLastError() != ERROR_FILE_NOT_FOUND) {
append_cleanup_error(message, "Delete temporary transaction journal", GetLastError());
}
return Result<void>{{Error{ErrorCode::service_error, message}}};
}
return Result<void>{};
}
Result<P104TransactionJournal> load_transaction_journal(const std::wstring& path) {
if (path.empty()) {
return Result<P104TransactionJournal>{{Error{
ErrorCode::invalid_argument,
"Transaction journal path must not be empty"}}};
}
HANDLE file = CreateFileW(
path.c_str(),
GENERIC_READ,
FILE_SHARE_READ,
nullptr,
OPEN_EXISTING,
FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
nullptr);
if (file == INVALID_HANDLE_VALUE) {
return Result<P104TransactionJournal>{{win32_error(
"Open transaction journal", GetLastError())}};
}
BY_HANDLE_FILE_INFORMATION information{};
if (!GetFileInformationByHandle(file, &information)) {
const auto error = GetLastError();
std::string message = win32_error("Inspect transaction journal", error).message;
close_file(file, message);
return Result<P104TransactionJournal>{{Error{
ErrorCode::service_error, message}}};
}
if ((information.dwFileAttributes &
(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
std::string message = "Transaction journal must be a regular file";
close_file(file, message);
return Result<P104TransactionJournal>{{Error{
ErrorCode::invalid_recovery_state,
message}}};
}
LARGE_INTEGER size{};
if (!GetFileSizeEx(file, &size)) {
const auto error = GetLastError();
std::string message = win32_error("Read transaction journal size", error).message;
close_file(file, message);
return Result<P104TransactionJournal>{{Error{
ErrorCode::service_error,
message}}};
}
if (size.QuadPart < 0 ||
static_cast<unsigned long long>(size.QuadPart) > maximum_journal_size) {
std::string message = "Transaction journal size is outside the permitted range";
close_file(file, message);
return Result<P104TransactionJournal>{{Error{
ErrorCode::invalid_recovery_state,
message}}};
}
std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));
std::size_t read_total = 0;
std::string read_failure;
while (read_total < bytes.size()) {
const auto remaining = bytes.size() - read_total;
DWORD read = 0;
if (!ReadFile(
file,
bytes.data() + read_total,
static_cast<DWORD>(remaining),
&read,
nullptr)) {
read_failure = win32_error("Read transaction journal", GetLastError()).message;
break;
}
if (read == 0) {
read_failure = "Transaction journal ended before the reported file size";
break;
}
read_total += read;
}
close_file(file, read_failure);
if (!read_failure.empty()) {
return Result<P104TransactionJournal>{{Error{
ErrorCode::service_error,
read_failure}}};
}
return parse_transaction_journal(bytes);
}
Result<void> clear_transaction_journal(const std::wstring& path) {
if (path.empty()) {
return Result<void>{{Error{
ErrorCode::invalid_argument,
"Transaction journal path must not be empty"}}};
}
if (!DeleteFileW(path.c_str())) {
const auto error = GetLastError();
if (error != ERROR_FILE_NOT_FOUND &&
error != ERROR_PATH_NOT_FOUND) {
return Result<void>{{win32_error("Remove transaction journal", error)}};
}
}
return Result<void>{};
}
}
