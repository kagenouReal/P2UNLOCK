#pragma once
#include "pseries/types.hpp"
#include <string>
namespace pseries::platform {
[[nodiscard]] Result<void> set_pci_device_enabled(
const std::wstring& instance_id,
bool enable,
bool wait_for_completion = true);
}
