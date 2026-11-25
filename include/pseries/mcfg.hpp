#pragma once
#include "pseries/types.hpp"
#include <cstdint>
#include <span>
#include <vector>
namespace pseries {
struct McfgAllocation {
std::uint64_t base_address{};
std::uint16_t segment{};
std::uint8_t start_bus{};
std::uint8_t end_bus{};
};
[[nodiscard]] Result<std::vector<McfgAllocation>> parse_mcfg_table(
std::span<const std::uint8_t> table);
[[nodiscard]] Result<PciBdf> resolve_mcfg_bdf(
std::span<const McfgAllocation> allocations,
const PciBusAddress& bus_address);
[[nodiscard]] Result<std::uint64_t> pci_ecam_address(
std::span<const McfgAllocation> allocations,
const PciBdf& bdf,
std::uint16_t register_offset,
std::uint8_t access_width);
}
