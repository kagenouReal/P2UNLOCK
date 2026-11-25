#pragma once
#include "pseries/types.hpp"
#include <cstdint>
#include <span>
namespace pseries {
constexpr std::uint16_t nvidia_vendor_id{0x10DE};
constexpr std::uint16_t p104_100_device_id{0x1B87};
constexpr std::uint16_t p104_101_device_id{0x1BC7};
[[nodiscard]] bool is_supported_unlock_gpu(
std::uint16_t vendor_id,
std::uint16_t device_id) noexcept;
[[nodiscard]] bool is_known_p_series_gpu(const HardwareIdentity& identity) noexcept;
[[nodiscard]] Result<HardwareIdentity> select_unique_p_series_gpu(
std::span<const HardwareIdentity> devices);
}
