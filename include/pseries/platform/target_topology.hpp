#pragma once
#include "pseries/mcfg.hpp"
#include "pseries/platform/pci_config.hpp"
#include "pseries/platform/pci_discovery.hpp"
#include <string>
#include <vector>
namespace pseries::platform {
struct TargetTopology {
PciDeviceRecord gpu;
PciDeviceRecord upstream;
PciBdf gpu_bdf;
PciBdf upstream_bdf;
std::uint16_t gpu_vendor_id{};
std::uint16_t gpu_device_id{};
std::uint16_t upstream_vendor_id{};
std::uint16_t upstream_device_id{};
std::vector<McfgAllocation> allocations;
};
[[nodiscard]] Result<TargetTopology> resolve_p104_target_topology(
const std::wstring& driver_path);
}
