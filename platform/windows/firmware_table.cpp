#include "pseries/platform/firmware_table.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <string>
#include <vector>
namespace pseries::platform {
namespace {
constexpr DWORD fourcc(char first, char second, char third, char fourth) noexcept {
return (static_cast<DWORD>(first) << 24) |
(static_cast<DWORD>(second) << 16) |
(static_cast<DWORD>(third) << 8) |
static_cast<DWORD>(fourth);
}
constexpr DWORD kAcpiProvider = fourcc('A', 'C', 'P', 'I');
constexpr DWORD kMcfgTable = fourcc('G', 'F', 'C', 'M');
Error firmware_error(const char* operation, DWORD error) {
return Error{
ErrorCode::service_error,
std::string(operation) + " failed with Win32 error " + std::to_string(error)};
}
}
Result<std::vector<McfgAllocation>> read_system_mcfg() {
const auto required_bytes = GetSystemFirmwareTable(
kAcpiProvider,
kMcfgTable,
nullptr,
0);
if (required_bytes == 0) {
return Result<std::vector<McfgAllocation>>{{firmware_error(
"Read ACPI MCFG table size", GetLastError())}};
}
std::vector<std::uint8_t> table(required_bytes);
const auto returned_bytes = GetSystemFirmwareTable(
kAcpiProvider,
kMcfgTable,
table.data(),
required_bytes);
if (returned_bytes == 0) {
return Result<std::vector<McfgAllocation>>{{firmware_error(
"Read ACPI MCFG table", GetLastError())}};
}
if (returned_bytes > required_bytes) {
return Result<std::vector<McfgAllocation>>{{Error{
ErrorCode::service_error,
"ACPI MCFG table changed size during retrieval; retry the read"}}};
}
table.resize(returned_bytes);
const auto parsed = parse_mcfg_table(table);
if (!parsed.has_value()) {
return Result<std::vector<McfgAllocation>>{{parsed.error()}};
}
return Result<std::vector<McfgAllocation>>{parsed.get()};
}
}
