#pragma once
#include "pseries/types.hpp"
#include <cstdint>
#include <functional>
namespace pseries {
using PciConfigDwordReader = std::function<Result<std::uint32_t>(std::uint16_t)>;
[[nodiscard]] Result<PcieLinkInformation> decode_pcie_link_registers(
std::uint32_t link_capability,
std::uint32_t link_status);
[[nodiscard]] Result<PcieLinkInformation> read_pcie_link_information(
const PciConfigDwordReader& read_config_dword);
}
