#pragma once
#include "pseries/types.hpp"
#include <string_view>
namespace pseries {
[[nodiscard]] Result<HardwareIdentity> parse_pci_hardware_id(std::string_view text);
[[nodiscard]] Result<PciBusAddress> parse_windows_pci_address(
std::uint32_t bus_number,
std::uint32_t address);
}
