#pragma once
#include "pseries/profile.hpp"
#include <string>
namespace pseries::platform {
inline constexpr wchar_t kDefaultDriverPath[] =
L"C:\\ProgramData\\P104-Gen2x8\\tool\\drivers\\SvIoCtrlx64.sys";
[[nodiscard]] std::wstring p104_profile_path();
[[nodiscard]] Result<P104ServiceProfile> load_p104_service_profile();
[[nodiscard]] Result<void> save_p104_service_profile(
const P104ServiceProfile& profile);
[[nodiscard]] Result<void> clear_p104_service_profile();
[[nodiscard]] std::wstring default_driver_path();
}
