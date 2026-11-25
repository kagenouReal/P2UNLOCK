#include "pseries/device_selection.hpp"
namespace pseries {
namespace {
constexpr bool is_known_device_id(std::uint16_t device_id) noexcept {
switch (device_id) {
case p104_100_device_id:
case p104_101_device_id:
return true;
default:
return false;
}
}
}
bool is_supported_unlock_gpu(
std::uint16_t vendor_id,
std::uint16_t device_id) noexcept {
return vendor_id == nvidia_vendor_id && is_known_device_id(device_id);
}
bool is_known_p_series_gpu(const HardwareIdentity& identity) noexcept {
return is_supported_unlock_gpu(identity.vendor_id, identity.device_id);
}
Result<HardwareIdentity> select_unique_p_series_gpu(std::span<const HardwareIdentity> devices) {
const HardwareIdentity* candidate = nullptr;
for (const auto& device : devices) {
if (!is_known_p_series_gpu(device)) {
continue;
}
if (candidate != nullptr) {
return Result<HardwareIdentity>{{Error{
ErrorCode::unsupported,
"Multiple known P-series GPUs found; refusing ambiguous selection"}}};
}
candidate = &device;
}
if (candidate == nullptr) {
return Result<HardwareIdentity>{{Error{
ErrorCode::hardware_not_found,
"No known P-series GPU found"}}};
}
if (!candidate->bdf || !candidate->bdf->is_valid()) {
return Result<HardwareIdentity>{{Error{
ErrorCode::unsupported,
"P-series GPU has no valid PCI address"}}};
}
return Result<HardwareIdentity>{{*candidate}};
}
}
