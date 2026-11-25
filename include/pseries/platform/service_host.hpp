#pragma once
#include "pseries/profile.hpp"
#include "pseries/types.hpp"
namespace pseries::platform {
[[nodiscard]] Result<void> apply_p104_service_profile(
const P104ServiceProfile& profile);
[[nodiscard]] Result<P104Profile> detect_current_p104_profile();
[[nodiscard]] Result<void> apply_p104_width(std::uint8_t lanes);
[[nodiscard]] Result<void> apply_p104_generation(
pseries::LinkGeneration generation);
[[nodiscard]] Result<void> run_p2unlock_service();
}
