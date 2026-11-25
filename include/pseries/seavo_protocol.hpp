#pragma once
#include <cstddef>
#include <cstdint>
namespace pseries::seavo {
inline constexpr std::uint32_t physical_read_ioctl = 0x9C406104;
inline constexpr std::uint32_t physical_write_ioctl = 0x9C40A108;
inline constexpr std::size_t physical_access_header_size = 16;
struct PhysicalAccessHeader {
std::uint64_t address{};
std::uint32_t element_size{};
std::uint32_t count{};
};
static_assert(sizeof(PhysicalAccessHeader) == physical_access_header_size);
static_assert(offsetof(PhysicalAccessHeader, address) == 0);
static_assert(offsetof(PhysicalAccessHeader, element_size) == 8);
static_assert(offsetof(PhysicalAccessHeader, count) == 12);
[[nodiscard]] constexpr bool is_supported_physical_width(std::uint32_t width) noexcept {
return width == 1 || width == 2 || width == 4;
}
[[nodiscard]] constexpr bool is_aligned_physical_access(
std::uint64_t address,
std::uint32_t width) noexcept {
return is_supported_physical_width(width) && address % width == 0;
}
}
