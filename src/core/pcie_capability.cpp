#include "pseries/pcie_capability.hpp"
#include <array>
#include <optional>
namespace pseries {
namespace {
constexpr std::uint16_t kConventionalConfigSize = 256;
Result<PcieLinkInformation> capability_error(ErrorCode code, const char* message) {
return Result<PcieLinkInformation>{{Error{code, message}}};
}
std::optional<LinkGeneration> decode_generation(std::uint32_t encoded) noexcept {
switch (encoded) {
case 1:
return LinkGeneration::gen1;
case 2:
return LinkGeneration::gen2;
case 3:
return LinkGeneration::gen3;
case 4:
return LinkGeneration::gen4;
case 5:
return LinkGeneration::gen5;
default:
return std::nullopt;
}
}
std::optional<LinkWidth> decode_width(std::uint32_t encoded) noexcept {
switch (encoded) {
case 1:
case 2:
case 4:
case 8:
case 12:
case 16:
case 32:
return LinkWidth{static_cast<std::uint8_t>(encoded)};
default:
return std::nullopt;
}
}
}
Result<PcieLinkInformation> decode_pcie_link_registers(
std::uint32_t link_capability,
std::uint32_t link_status) {
const auto maximum_generation = decode_generation(link_capability & 0x0Fu);
const auto maximum_width = decode_width((link_capability >> 4) & 0x3Fu);
const auto current_generation = decode_generation(link_status & 0x0Fu);
const auto current_width = decode_width((link_status >> 4) & 0x3Fu);
if (!maximum_generation || !maximum_width ||
!current_generation || !current_width) {
return Result<PcieLinkInformation>{{Error{
ErrorCode::unsupported,
"PCIe Link Capability or Link Status contains a reserved speed or width"}}};
}
return Result<PcieLinkInformation>{{PcieLinkInformation{
*current_generation,
*current_width,
*maximum_generation,
*maximum_width,
(link_status & (1u << 11)) != 0
? LinkTrainingState::training
: LinkTrainingState::stable}}};
}
Result<PcieLinkInformation> read_pcie_link_information(
const PciConfigDwordReader& read_config_dword) {
if (!read_config_dword) {
return capability_error(
ErrorCode::invalid_argument,
"PCI configuration reader is empty");
}
const auto status = read_config_dword(0x04);
if (!status.has_value()) {
return Result<PcieLinkInformation>{{status.error()}};
}
if ((status.get() & (1u << 20)) == 0) {
return capability_error(
ErrorCode::unsupported,
"PCI device does not advertise a standard capability list");
}
const auto header = read_config_dword(0x0C);
if (!header.has_value()) {
return Result<PcieLinkInformation>{{header.error()}};
}
const auto header_type = (header.get() >> 16) & 0x7Fu;
if (header_type > 1) {
return capability_error(
ErrorCode::unsupported,
"PCI header type does not use a standard capability pointer");
}
const auto pointer_register = read_config_dword(0x34);
if (!pointer_register.has_value()) {
return Result<PcieLinkInformation>{{pointer_register.error()}};
}
auto capability_offset = static_cast<std::uint16_t>(pointer_register.get() & 0xFFu);
if (capability_offset == 0) {
return capability_error(
ErrorCode::unsupported,
"PCI device has no standard capabilities");
}
std::array<bool, kConventionalConfigSize / 4> visited{};
for (std::size_t count = 0; capability_offset != 0; ++count) {
if (count >= visited.size() ||
capability_offset < 0x40 ||
capability_offset > 0xFC ||
(capability_offset & 0x03u) != 0) {
return capability_error(
ErrorCode::invalid_configuration,
"PCI capability list contains an invalid offset");
}
const auto slot = capability_offset / 4;
if (visited[slot]) {
return capability_error(
ErrorCode::invalid_configuration,
"PCI capability list contains a cycle");
}
visited[slot] = true;
const auto capability = read_config_dword(capability_offset);
if (!capability.has_value()) {
return Result<PcieLinkInformation>{{capability.error()}};
}
const auto capability_id = capability.get() & 0xFFu;
const auto next_offset = static_cast<std::uint16_t>(
(capability.get() >> 8) & 0xFFu);
if (capability_id == 0x10) {
if (capability_offset > 0xEC ||
capability_offset + 0x30 > 0xFF) {
return capability_error(
ErrorCode::invalid_configuration,
"PCIe capability extends beyond conventional configuration space");
}
const auto link_capability = read_config_dword(
static_cast<std::uint16_t>(capability_offset + 0x0C));
if (!link_capability.has_value()) {
return Result<PcieLinkInformation>{{link_capability.error()}};
}
const auto link_control_status = read_config_dword(
static_cast<std::uint16_t>(capability_offset + 0x10));
if (!link_control_status.has_value()) {
return Result<PcieLinkInformation>{{link_control_status.error()}};
}
auto information = decode_pcie_link_registers(
link_capability.get(),
link_control_status.get() >> 16);
if (!information.has_value()) {
return information;
}
information.get().capability_offset = capability_offset;
information.get().link_control_offset =
static_cast<std::uint16_t>(capability_offset + 0x10);
information.get().link_control_2_offset =
static_cast<std::uint16_t>(capability_offset + 0x30);
return information;
}
if (next_offset != 0 &&
(next_offset < 0x40 ||
next_offset > 0xFC ||
(next_offset & 0x03u) != 0)) {
return capability_error(
ErrorCode::invalid_configuration,
"PCI capability list contains an invalid next offset");
}
capability_offset = next_offset;
}
return capability_error(
ErrorCode::unsupported,
"PCIe Link capability was not found");
}
}
