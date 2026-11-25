#pragma once
#include "pseries/mcfg.hpp"
#include "pseries/platform/seavo_backend.hpp"
#include "pseries/types.hpp"
#include <span>
namespace pseries::platform {
struct PciLinkInspection {
std::uint16_t vendor_id{};
std::uint16_t device_id{};
PcieLinkInformation link;
};
struct PciUpstreamBridge {
PciBdf bdf;
std::uint16_t vendor_id{};
std::uint16_t device_id{};
};
[[nodiscard]] Result<PciLinkInspection> inspect_pcie_link(
const SeavoBackend& backend,
std::span<const McfgAllocation> allocations,
const PciBdf& bdf);
[[nodiscard]] Result<PciUpstreamBridge> find_upstream_pcie_bridge(
const SeavoBackend& backend,
std::span<const McfgAllocation> allocations,
const PciBdf& gpu_bdf);
}
