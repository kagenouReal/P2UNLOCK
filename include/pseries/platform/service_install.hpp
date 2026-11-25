#pragma once
#include "pseries/types.hpp"
#include <string>
namespace pseries::platform {
inline constexpr wchar_t kP2UnlockServiceName[] = L"P2UNLOCK";
[[nodiscard]] Result<void> install_p2unlock_service(
const std::wstring& exe_path,
const std::wstring& driver_path);
[[nodiscard]] Result<void> uninstall_p2unlock_service();
[[nodiscard]] Result<void> start_p2unlock_service();
[[nodiscard]] Result<void> stop_p2unlock_service();
}
