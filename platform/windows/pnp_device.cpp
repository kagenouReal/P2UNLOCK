#include "pseries/platform/pnp_device.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <SetupAPI.h>
#include <cfgmgr32.h>
#include <string>
#include <vector>
namespace pseries::platform {
namespace {
constexpr DWORD kPnpTimeoutMs = 30000;
constexpr DWORD kPnpPollMs = 200;
constexpr ULONG kCmProbDisabled = 22;
constexpr ULONG kDnHasProblem = 0x00000400;
struct DeviceStatus {
bool disabled{};
};
class DeviceInfoSet final {
public:
explicit DeviceInfoSet(HDEVINFO value) noexcept : value_(value) {
}
DeviceInfoSet(const DeviceInfoSet&) = delete;
DeviceInfoSet& operator=(const DeviceInfoSet&) = delete;
~DeviceInfoSet() {
if (value_ != INVALID_HANDLE_VALUE) {
SetupDiDestroyDeviceInfoList(value_);
}
}
[[nodiscard]] HDEVINFO get() const noexcept {
return value_;
}
private:
HDEVINFO value_;
};
Error win32_error(const char* operation, DWORD error) {
return Error{
ErrorCode::service_error,
std::string(operation) + " failed with Win32 error " + std::to_string(error)};
}
Result<std::wstring> device_instance_id(HDEVINFO devices, SP_DEVINFO_DATA& device) {
DWORD required = 0;
if (SetupDiGetDeviceInstanceIdW(devices, &device, nullptr, 0, &required)) {
return Result<std::wstring>{{Error{
ErrorCode::service_error,
"Windows returned an empty PCI device instance ID"}}};
}
if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) {
return Result<std::wstring>{{win32_error(
"Read PCI device instance ID", GetLastError())}};
}
std::wstring value(required, L'\0');
if (!SetupDiGetDeviceInstanceIdW(
devices, &device, value.data(), required, &required)) {
return Result<std::wstring>{{win32_error(
"Read PCI device instance ID", GetLastError())}};
}
const auto terminator = value.find(L'\0');
if (terminator == std::wstring::npos) {
return Result<std::wstring>{{Error{
ErrorCode::service_error,
"Windows returned a non-terminated PCI device instance ID"}}};
}
value.resize(terminator);
return Result<std::wstring>{std::move(value)};
}
Result<bool> find_device(
HDEVINFO devices,
const std::wstring& instance_id,
SP_DEVINFO_DATA& match) {
for (DWORD index = 0;; ++index) {
SP_DEVINFO_DATA device{};
device.cbSize = sizeof(device);
if (!SetupDiEnumDeviceInfo(devices, index, &device)) {
const auto error = GetLastError();
if (error == ERROR_NO_MORE_ITEMS) {
return Result<bool>{false};
}
return Result<bool>{{win32_error("Enumerate PCI device entry", error)}};
}
const auto current_id = device_instance_id(devices, device);
if (!current_id.has_value()) {
return Result<bool>{{current_id.error()}};
}
if (_wcsicmp(current_id.get().c_str(), instance_id.c_str()) == 0) {
match = device;
return Result<bool>{true};
}
}
}
Result<DeviceStatus> read_status(DEVINST devnode) {
ULONG status = 0;
ULONG problem = 0;
const auto result = CM_Get_DevNode_Status(&status, &problem, devnode, 0);
if (result != CR_SUCCESS) {
return Result<DeviceStatus>{{Error{
ErrorCode::service_error,
"Read PCI device status failed with CONFIGRET " +
std::to_string(static_cast<unsigned long>(result))}}};
}
return Result<DeviceStatus>{DeviceStatus{
(status & kDnHasProblem) != 0 && problem == kCmProbDisabled}};
}
Result<void> change_state(HDEVINFO devices, SP_DEVINFO_DATA& device, bool enable) {
SP_PROPCHANGE_PARAMS params{};
params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
params.StateChange = enable ? DICS_ENABLE : DICS_DISABLE;
params.Scope = DICS_FLAG_GLOBAL;
params.HwProfile = 0;
if (!SetupDiSetClassInstallParamsW(
devices,
&device,
reinterpret_cast<SP_CLASSINSTALL_HEADER*>(&params),
sizeof(params))) {
return Result<void>{{win32_error(
enable ? "Set PCI device enable parameters" : "Set PCI device disable parameters",
GetLastError())}};
}
if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devices, &device)) {
return Result<void>{{win32_error(
enable ? "Enable PCI device" : "Disable PCI device",
GetLastError())}};
}
return Result<void>{};
}
}
Result<void> set_pci_device_enabled(
const std::wstring& instance_id,
bool enable,
bool wait_for_completion) {
if (instance_id.empty()) {
return Result<void>{{Error{
ErrorCode::invalid_argument,
"PCI device instance ID is empty"}}};
}
const DeviceInfoSet devices(SetupDiGetClassDevsW(
nullptr,
L"PCI",
nullptr,
DIGCF_ALLCLASSES | DIGCF_PRESENT));
if (devices.get() == INVALID_HANDLE_VALUE) {
return Result<void>{{win32_error(
"Enumerate PCI devices", GetLastError())}};
}
SP_DEVINFO_DATA device{};
device.cbSize = sizeof(device);
const auto found = find_device(devices.get(), instance_id, device);
if (!found.has_value()) {
return Result<void>{{found.error()}};
}
if (!found.get()) {
return Result<void>{{Error{
ErrorCode::hardware_not_found,
"The requested PCI device is not present"}}};
}
const auto initial = read_status(device.DevInst);
if (!initial.has_value()) {
return Result<void>{{initial.error()}};
}
const auto changed = change_state(devices.get(), device, enable);
if (!changed.has_value()) {
return changed;
}
if (!wait_for_completion) {
return Result<void>{};
}
const auto deadline = GetTickCount64() + kPnpTimeoutMs;
for (;;) {
const auto status = read_status(device.DevInst);
if (!status.has_value()) {
return Result<void>{{status.error()}};
}
if (status.get().disabled == !enable) {
return Result<void>{};
}
if (GetTickCount64() >= deadline) {
return Result<void>{{Error{
ErrorCode::service_error,
enable
? "PCI device did not become enabled before the timeout"
: "PCI device did not become disabled before the timeout"}}};
}
Sleep(kPnpPollMs);
}
}
}
