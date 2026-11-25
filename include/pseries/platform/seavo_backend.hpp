#pragma once
#include "pseries/types.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstdint>
#include <memory>
#include <string>
namespace pseries::platform {
class SeavoBackend final {
public:
[[nodiscard]] static Result<std::unique_ptr<SeavoBackend>> load(
const std::wstring& driver_path);
SeavoBackend(const SeavoBackend&) = delete;
SeavoBackend& operator=(const SeavoBackend&) = delete;
~SeavoBackend();
[[nodiscard]] Result<void> close();
[[nodiscard]] Result<std::uint32_t> read_physical(
std::uint64_t address,
std::uint32_t width) const;
[[nodiscard]] Result<void> write_physical(
std::uint64_t address,
std::uint32_t width,
std::uint32_t value) const;
private:
SeavoBackend(HANDLE device, SC_HANDLE manager, SC_HANDLE service) noexcept;
HANDLE device_{INVALID_HANDLE_VALUE};
SC_HANDLE manager_{};
SC_HANDLE service_{};
};
}
