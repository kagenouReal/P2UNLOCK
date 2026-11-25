#pragma once
#include "pseries/types.hpp"
#include <cstdint>
#include <string>
#include <string_view>
namespace pseries {
enum class P104Profile : std::uint8_t {
gen1_4x,
gen1_8x,
gen1_16x,
gen2_4x,
gen2_8x,
gen2_16x,
};
struct P104ServiceProfile {
std::uint32_t schema_version{2};
P104Profile profile{P104Profile::gen2_8x};
bool decorative_x16{true};
std::string driver_path;
};
[[nodiscard]] const char* p104_profile_name(P104Profile profile) noexcept;
[[nodiscard]] Result<P104Profile> parse_p104_profile(std::string_view name) noexcept;
[[nodiscard]] Result<std::uint8_t> p104_profile_lanes(P104Profile profile) noexcept;
[[nodiscard]] Result<LinkGeneration> p104_profile_generation(
P104Profile profile) noexcept;
[[nodiscard]] Result<P104Profile> make_p104_profile(
LinkGeneration generation,
std::uint8_t lanes) noexcept;
[[nodiscard]] Result<P104ServiceProfile> parse_p104_service_profile(
std::string_view text) noexcept;
[[nodiscard]] Result<std::string> serialize_p104_service_profile(
const P104ServiceProfile& profile);
}
