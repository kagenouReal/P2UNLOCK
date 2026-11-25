#include "pseries/mcfg.hpp"
#include <limits>
#include <optional>
#include <utility>
namespace pseries {
namespace {
constexpr std::size_t kMcfgHeaderSize = 44;
constexpr std::size_t kAllocationSize = 16;
constexpr std::uint64_t kBytesPerBus = 1ull << 20;
constexpr std::uint16_t kConfigSpaceSize = 4096;
std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
return static_cast<std::uint16_t>(
static_cast<std::uint16_t>(bytes[offset]) |
(static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}
std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
std::uint32_t value = 0;
for (std::size_t index = 0; index < 4; ++index) {
value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8);
}
return value;
}
std::uint64_t read_u64(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
std::uint64_t value = 0;
for (std::size_t index = 0; index < 8; ++index) {
value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8);
}
return value;
}
bool has_valid_checksum(std::span<const std::uint8_t> table) noexcept {
std::uint8_t sum = 0;
for (const auto byte : table) {
sum = static_cast<std::uint8_t>(sum + byte);
}
return sum == 0;
}
bool is_valid_allocation(const McfgAllocation& allocation) noexcept {
if (allocation.start_bus > allocation.end_bus ||
(allocation.base_address & (kBytesPerBus - 1)) != 0) {
return false;
}
const auto bus_count = static_cast<std::uint64_t>(
allocation.end_bus - allocation.start_bus + 1);
const auto region_size = bus_count * kBytesPerBus;
return allocation.base_address <=
std::numeric_limits<std::uint64_t>::max() - (region_size - 1);
}
bool bus_ranges_overlap(
const McfgAllocation& first,
const McfgAllocation& second) noexcept {
return first.segment == second.segment &&
first.start_bus <= second.end_bus &&
second.start_bus <= first.end_bus;
}
Error configuration_error(std::string message) {
return Error{ErrorCode::invalid_configuration, std::move(message)};
}
}
Result<std::vector<McfgAllocation>> parse_mcfg_table(
std::span<const std::uint8_t> table) {
if (table.size() < kMcfgHeaderSize) {
return Result<std::vector<McfgAllocation>>{{configuration_error(
"ACPI MCFG table is shorter than its fixed header")}};
}
if (table[0] != 'M' || table[1] != 'C' || table[2] != 'F' || table[3] != 'G') {
return Result<std::vector<McfgAllocation>>{{configuration_error(
"ACPI table signature is not MCFG")}};
}
if (read_u32(table, 4) != table.size()) {
return Result<std::vector<McfgAllocation>>{{configuration_error(
"ACPI MCFG length does not match the supplied table")}};
}
if ((table.size() - kMcfgHeaderSize) % kAllocationSize != 0) {
return Result<std::vector<McfgAllocation>>{{configuration_error(
"ACPI MCFG table has a truncated allocation entry")}};
}
if (!has_valid_checksum(table)) {
return Result<std::vector<McfgAllocation>>{{configuration_error(
"ACPI MCFG checksum is invalid")}};
}
for (std::size_t offset = 36; offset < kMcfgHeaderSize; ++offset) {
if (table[offset] != 0) {
return Result<std::vector<McfgAllocation>>{{configuration_error(
"ACPI MCFG reserved header bytes must be zero")}};
}
}
std::vector<McfgAllocation> allocations;
allocations.reserve((table.size() - kMcfgHeaderSize) / kAllocationSize);
for (std::size_t offset = kMcfgHeaderSize; offset < table.size();
offset += kAllocationSize) {
const McfgAllocation allocation{
read_u64(table, offset),
read_u16(table, offset + 8),
table[offset + 10],
table[offset + 11]};
if (read_u32(table, offset + 12) != 0) {
return Result<std::vector<McfgAllocation>>{{configuration_error(
"ACPI MCFG allocation reserved field must be zero")}};
}
if (!is_valid_allocation(allocation)) {
return Result<std::vector<McfgAllocation>>{{configuration_error(
"ACPI MCFG allocation has an invalid base address or bus range")}};
}
for (const auto& existing : allocations) {
if (bus_ranges_overlap(existing, allocation)) {
return Result<std::vector<McfgAllocation>>{{configuration_error(
"ACPI MCFG allocations overlap within a PCI segment")}};
}
}
allocations.push_back(allocation);
}
return Result<std::vector<McfgAllocation>>{{std::move(allocations)}};
}
Result<PciBdf> resolve_mcfg_bdf(
std::span<const McfgAllocation> allocations,
const PciBusAddress& bus_address) {
const PciBdf candidate{0, bus_address.bus, bus_address.device, bus_address.function};
if (!candidate.is_valid()) {
return Result<PciBdf>{{Error{
ErrorCode::invalid_argument,
"PnP PCI device/function address is outside PCI limits"}}};
}
std::optional<PciBdf> resolved;
for (const auto& allocation : allocations) {
if (bus_address.bus < allocation.start_bus ||
bus_address.bus > allocation.end_bus) {
continue;
}
if (!is_valid_allocation(allocation)) {
return Result<PciBdf>{{configuration_error(
"Matching MCFG allocation has an invalid base address or bus range")}};
}
if (resolved) {
return Result<PciBdf>{{configuration_error(
"PnP bus location matches multiple PCI segments")}};
}
resolved = PciBdf{
allocation.segment,
bus_address.bus,
bus_address.device,
bus_address.function};
}
if (!resolved) {
return Result<PciBdf>{{configuration_error(
"No MCFG allocation covers the PnP bus location")}};
}
return Result<PciBdf>{*resolved};
}
Result<std::uint64_t> pci_ecam_address(
std::span<const McfgAllocation> allocations,
const PciBdf& bdf,
std::uint16_t register_offset,
std::uint8_t access_width) {
if (!bdf.is_valid()) {
return Result<std::uint64_t>{{Error{
ErrorCode::invalid_argument,
"PCI BDF is outside device or function limits"}}};
}
if (access_width != 1 && access_width != 2 && access_width != 4) {
return Result<std::uint64_t>{{Error{
ErrorCode::invalid_argument,
"PCI configuration access width must be 1, 2, or 4 bytes"}}};
}
if (register_offset >= kConfigSpaceSize ||
register_offset % access_width != 0 ||
access_width > kConfigSpaceSize - register_offset) {
return Result<std::uint64_t>{{Error{
ErrorCode::invalid_argument,
"PCI configuration access is unaligned or outside the 4-KiB space"}}};
}
const McfgAllocation* match = nullptr;
for (const auto& allocation : allocations) {
if (allocation.segment != bdf.domain ||
bdf.bus < allocation.start_bus ||
bdf.bus > allocation.end_bus) {
continue;
}
if (match != nullptr) {
return Result<std::uint64_t>{{configuration_error(
"PCI BDF matches multiple MCFG allocations")}};
}
if (!is_valid_allocation(allocation)) {
return Result<std::uint64_t>{{configuration_error(
"Matching MCFG allocation has an invalid base address or bus range")}};
}
match = &allocation;
}
if (match == nullptr) {
return Result<std::uint64_t>{{configuration_error(
"No MCFG allocation covers the requested PCI BDF")}};
}
const auto bus_offset =
static_cast<std::uint64_t>(bdf.bus - match->start_bus) * kBytesPerBus;
const auto device_offset = static_cast<std::uint64_t>(bdf.device) << 15;
const auto function_offset = static_cast<std::uint64_t>(bdf.function) << 12;
return Result<std::uint64_t>{{
match->base_address + bus_offset + device_offset + function_offset + register_offset}};
}
}
