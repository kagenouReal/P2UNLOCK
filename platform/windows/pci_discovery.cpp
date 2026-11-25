#include "pseries/platform/pci_discovery.hpp"
#include "pseries/pci_identity.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <SetupAPI.h>
#include <string>
#include <utility>
#include <vector>
namespace pseries::platform {
namespace {
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
Result<std::wstring> get_device_instance_id(HDEVINFO devices, SP_DEVINFO_DATA& device) {
DWORD required = 0;
if (SetupDiGetDeviceInstanceIdW(devices, &device, nullptr, 0, &required)) {
return Result<std::wstring>{{Error{
ErrorCode::service_error,
"Windows returned an empty PCI device instance ID"}}};
}
if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) {
return Result<std::wstring>{{win32_error("Read PCI device instance ID", GetLastError())}};
}
std::wstring instance_id(required, L'\0');
if (!SetupDiGetDeviceInstanceIdW(
devices,
&device,
instance_id.data(),
required,
&required)) {
return Result<std::wstring>{{win32_error("Read PCI device instance ID", GetLastError())}};
}
const auto terminator = instance_id.find(L'\0');
if (terminator == std::wstring::npos) {
return Result<std::wstring>{{Error{
ErrorCode::service_error,
"Windows returned a non-terminated PCI device instance ID"}}};
}
instance_id.resize(terminator);
return Result<std::wstring>{std::move(instance_id)};
}
Result<std::wstring> get_optional_device_property(
HDEVINFO devices,
SP_DEVINFO_DATA& device,
DWORD property,
const char* property_name) {
DWORD property_type = 0;
DWORD required_bytes = 0;
if (!SetupDiGetDeviceRegistryPropertyW(
devices,
&device,
property,
&property_type,
nullptr,
0,
&required_bytes)) {
const auto error = GetLastError();
if (error == ERROR_INVALID_DATA) {
return Result<std::wstring>{std::wstring{}};
}
if (error != ERROR_INSUFFICIENT_BUFFER || required_bytes == 0) {
return Result<std::wstring>{{win32_error(property_name, error)}};
}
}
std::vector<wchar_t> buffer(required_bytes / sizeof(wchar_t) + 1, L'\0');
if (!SetupDiGetDeviceRegistryPropertyW(
devices,
&device,
property,
&property_type,
reinterpret_cast<PBYTE>(buffer.data()),
static_cast<DWORD>(buffer.size() * sizeof(wchar_t)),
&required_bytes)) {
return Result<std::wstring>{{win32_error(property_name, GetLastError())}};
}
if (property_type != REG_SZ && property_type != REG_EXPAND_SZ) {
return Result<std::wstring>{{Error{
ErrorCode::unsupported,
std::string(property_name) + " is not a string property"}}};
}
return Result<std::wstring>{std::wstring(buffer.data())};
}
Result<DWORD> get_required_device_dword_property(
HDEVINFO devices,
SP_DEVINFO_DATA& device,
DWORD property,
const char* property_name) {
DWORD property_type = 0;
DWORD required_bytes = 0;
if (SetupDiGetDeviceRegistryPropertyW(
devices,
&device,
property,
&property_type,
nullptr,
0,
&required_bytes)) {
return Result<DWORD>{{Error{
ErrorCode::service_error,
std::string(property_name) + " unexpectedly returned an empty value"}}};
}
const auto first_error = GetLastError();
if (first_error != ERROR_INSUFFICIENT_BUFFER || required_bytes != sizeof(DWORD)) {
return Result<DWORD>{{win32_error(property_name, first_error)}};
}
DWORD value = 0;
DWORD returned_bytes = 0;
if (!SetupDiGetDeviceRegistryPropertyW(
devices,
&device,
property,
&property_type,
reinterpret_cast<PBYTE>(&value),
sizeof(value),
&returned_bytes)) {
return Result<DWORD>{{win32_error(property_name, GetLastError())}};
}
if (property_type != REG_DWORD || returned_bytes != sizeof(value)) {
return Result<DWORD>{{Error{
ErrorCode::unsupported,
std::string(property_name) + " is not a DWORD property"}}};
}
return Result<DWORD>{value};
}
Result<std::string> hardware_id_to_ascii(const std::wstring& instance_id) {
std::string result;
result.reserve(instance_id.size());
for (const auto character : instance_id) {
if (character > 0x7F) {
return Result<std::string>{{Error{
ErrorCode::invalid_argument,
"PCI instance ID contains non-ASCII characters"}}};
}
result.push_back(static_cast<char>(character));
}
return Result<std::string>{std::move(result)};
}
}
Result<std::vector<PciDeviceRecord>> enumerate_present_pci_devices() {
const DeviceInfoSet devices(SetupDiGetClassDevsW(
nullptr,
L"PCI",
nullptr,
DIGCF_ALLCLASSES | DIGCF_PRESENT));
if (devices.get() == INVALID_HANDLE_VALUE) {
return Result<std::vector<PciDeviceRecord>>{{win32_error(
"Enumerate present PCI devices", GetLastError())}};
}
std::vector<PciDeviceRecord> records;
for (DWORD index = 0;; ++index) {
SP_DEVINFO_DATA device{};
device.cbSize = sizeof(device);
if (!SetupDiEnumDeviceInfo(devices.get(), index, &device)) {
const auto error = GetLastError();
if (error == ERROR_NO_MORE_ITEMS) {
break;
}
return Result<std::vector<PciDeviceRecord>>{{win32_error(
"Enumerate PCI device entry", error)}};
}
const auto instance_id = get_device_instance_id(devices.get(), device);
if (!instance_id.has_value()) {
return Result<std::vector<PciDeviceRecord>>{{instance_id.error()}};
}
const auto ascii_id = hardware_id_to_ascii(instance_id.get());
if (!ascii_id.has_value()) {
return Result<std::vector<PciDeviceRecord>>{{ascii_id.error()}};
}
const auto identity = parse_pci_hardware_id(ascii_id.get());
if (!identity.has_value()) {
return Result<std::vector<PciDeviceRecord>>{{identity.error()}};
}
const auto bus_number = get_required_device_dword_property(
devices.get(),
device,
SPDRP_BUSNUMBER,
"Read PCI bus number");
if (!bus_number.has_value()) {
return Result<std::vector<PciDeviceRecord>>{{bus_number.error()}};
}
const auto address = get_required_device_dword_property(
devices.get(),
device,
SPDRP_ADDRESS,
"Read PCI device/function address");
if (!address.has_value()) {
return Result<std::vector<PciDeviceRecord>>{{address.error()}};
}
const auto bus_address = parse_windows_pci_address(bus_number.get(), address.get());
if (!bus_address.has_value()) {
return Result<std::vector<PciDeviceRecord>>{{bus_address.error()}};
}
const auto description = get_optional_device_property(
devices.get(),
device,
SPDRP_DEVICEDESC,
"Read PCI device description");
if (!description.has_value()) {
return Result<std::vector<PciDeviceRecord>>{{description.error()}};
}
const auto location = get_optional_device_property(
devices.get(),
device,
SPDRP_LOCATION_INFORMATION,
"Read PCI device location");
if (!location.has_value()) {
return Result<std::vector<PciDeviceRecord>>{{location.error()}};
}
records.push_back(PciDeviceRecord{
identity.get(),
bus_address.get(),
instance_id.get(),
description.get(),
location.get()});
}
return Result<std::vector<PciDeviceRecord>>{std::move(records)};
}
}
