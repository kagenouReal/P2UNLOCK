#pragma once
#include "pseries/types.hpp"
#include <string>
#include <vector>
namespace pseries::platform {
struct PciDeviceRecord {
HardwareIdentity identity;
PciBusAddress bus_address;
std::wstring instance_id;
std::wstring description;
std::wstring location;
};
[[nodiscard]] Result<std::vector<PciDeviceRecord>> enumerate_present_pci_devices();
}
