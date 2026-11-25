#include "pseries/platform/pci_config.hpp"
#include "pseries/pcie_capability.hpp"
#include <optional>
namespace pseries::platform {
namespace {
Result<std::uint32_t> read_config_value(
const SeavoBackend& backend,
std::span<const McfgAllocation> allocations,
const PciBdf& bdf,
std::uint16_t offset,
std::uint8_t width) {
const auto address = pci_ecam_address(allocations, bdf, offset, width);
if (!address.has_value()) {
return Result<std::uint32_t>{{address.error()}};
}
return backend.read_physical(address.get(), width);
}
bool absent_device(std::uint32_t value) noexcept {
return value == 0 || value == 0xFFFFFFFFu;
}
}
Result<PciLinkInspection> inspect_pcie_link(
const SeavoBackend& backend,
std::span<const McfgAllocation> allocations,
const PciBdf& bdf) {
const auto read_dword = [&backend, allocations, &bdf](
std::uint16_t offset) -> Result<std::uint32_t> {
return read_config_value(backend, allocations, bdf, offset, 4);
};
const auto identity = read_dword(0);
if (!identity.has_value()) {
return Result<PciLinkInspection>{{identity.error()}};
}
const auto vendor_id = static_cast<std::uint16_t>(identity.get() & 0xFFFFu);
const auto device_id = static_cast<std::uint16_t>(identity.get() >> 16);
if (vendor_id == 0xFFFFu || device_id == 0xFFFFu) {
return Result<PciLinkInspection>{{Error{
ErrorCode::hardware_not_found,
"PCI configuration space reports no device at the resolved BDF"}}};
}
const auto link = read_pcie_link_information(read_dword);
if (!link.has_value()) {
return Result<PciLinkInspection>{{link.error()}};
}
return Result<PciLinkInspection>{{PciLinkInspection{
vendor_id,
device_id,
link.get()}}};
}
Result<PciUpstreamBridge> find_upstream_pcie_bridge(
const SeavoBackend& backend,
std::span<const McfgAllocation> allocations,
const PciBdf& gpu_bdf) {
if (!gpu_bdf.is_valid()) {
return Result<PciUpstreamBridge>{{Error{
ErrorCode::invalid_argument,
"GPU PCI BDF is invalid"}}};
}
std::optional<PciUpstreamBridge> matched;
for (std::uint16_t bus = 0; bus <= gpu_bdf.bus; ++bus) {
for (std::uint32_t device = 0; device < 32; ++device) {
for (std::uint32_t function = 0; function < 8; ++function) {
const PciBdf candidate{
gpu_bdf.domain,
static_cast<std::uint8_t>(bus),
static_cast<std::uint8_t>(device),
static_cast<std::uint8_t>(function)};
if (candidate.bus == gpu_bdf.bus &&
candidate.device == gpu_bdf.device &&
candidate.function == gpu_bdf.function) {
continue;
}
const auto identity = read_config_value(
backend, allocations, candidate, 0x00, 4);
if (!identity.has_value() || absent_device(identity.get())) {
continue;
}
const auto header_word = read_config_value(
backend, allocations, candidate, 0x0C, 4);
if (!header_word.has_value()) {
continue;
}
const auto header = (header_word.get() >> 16) & 0xFFu;
if ((header & 0x7Fu) != 0x01u) {
continue;
}
const auto buses = read_config_value(
backend, allocations, candidate, 0x18, 4);
if (!buses.has_value()) {
continue;
}
const auto primary_bus = static_cast<std::uint8_t>(buses.get() & 0xFFu);
const auto secondary_bus =
static_cast<std::uint8_t>((buses.get() >> 8) & 0xFFu);
const auto subordinate_bus =
static_cast<std::uint8_t>((buses.get() >> 16) & 0xFFu);
if (primary_bus != candidate.bus ||
secondary_bus <= primary_bus ||
secondary_bus != gpu_bdf.bus ||
subordinate_bus < gpu_bdf.bus) {
continue;
}
const PciUpstreamBridge bridge{
candidate,
static_cast<std::uint16_t>(identity.get() & 0xFFFFu),
static_cast<std::uint16_t>(identity.get() >> 16)};
if (bridge.vendor_id == 0xFFFFu || bridge.device_id == 0xFFFFu) {
continue;
}
if (matched.has_value()) {
return Result<PciUpstreamBridge>{{Error{
ErrorCode::unsupported,
"Multiple upstream PCI bridges claim the GPU bus"}}};
}
matched = bridge;
}
}
}
if (!matched.has_value()) {
return Result<PciUpstreamBridge>{{Error{
ErrorCode::hardware_not_found,
"No upstream PCI bridge was found for the GPU bus"}}};
}
return Result<PciUpstreamBridge>{*matched};
}
}
