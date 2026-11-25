#pragma once
#include "pseries/mcfg.hpp"
#include <vector>
namespace pseries::platform {
[[nodiscard]] Result<std::vector<McfgAllocation>> read_system_mcfg();
}
