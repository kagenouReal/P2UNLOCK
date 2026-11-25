#include "pseries/pci_identity.hpp"
#include <optional>
namespace pseries {
namespace {
char ascii_upper(char value) noexcept {
if (value >= 'a' && value <= 'z') {
return static_cast<char>(value - ('a' - 'A'));
}
return value;
}
bool starts_with_ascii_case_insensitive(std::string_view text, std::string_view prefix) noexcept {
if (text.size() < prefix.size()) {
return false;
}
for (std::size_t index = 0; index < prefix.size(); ++index) {
if (ascii_upper(text[index]) != ascii_upper(prefix[index])) {
return false;
}
}
return true;
}
std::optional<std::size_t> find_ascii_case_insensitive(
std::string_view text,
std::string_view token) noexcept {
if (token.size() > text.size()) {
return std::nullopt;
}
for (std::size_t start = 0; start <= text.size() - token.size(); ++start) {
if (starts_with_ascii_case_insensitive(text.substr(start), token)) {
return start;
}
}
return std::nullopt;
}
int hex_value(char value) noexcept {
if (value >= '0' && value <= '9') {
return value - '0';
}
value = ascii_upper(value);
if (value >= 'A' && value <= 'F') {
return value - 'A' + 10;
}
return -1;
}
bool is_field_delimiter(std::string_view text, std::size_t position) noexcept {
return position == text.size() || text[position] == '&' || text[position] == '\\';
}
std::optional<std::uint16_t> parse_hex_at(
std::string_view text,
std::size_t first_digit,
bool& malformed) noexcept {
if (first_digit + 4 > text.size()) {
malformed = true;
return std::nullopt;
}
std::uint16_t value = 0;
for (std::size_t index = first_digit; index < first_digit + 4; ++index) {
const auto digit = hex_value(text[index]);
if (digit < 0) {
malformed = true;
return std::nullopt;
}
value = static_cast<std::uint16_t>((value << 4) | static_cast<std::uint16_t>(digit));
}
return value;
}
std::optional<std::uint16_t> parse_hex_field(
std::string_view text,
std::string_view marker,
bool& malformed) noexcept {
const auto position = find_ascii_case_insensitive(text, marker);
if (!position) {
return std::nullopt;
}
if (*position == 0 || (text[*position - 1] != '\\' && text[*position - 1] != '&')) {
malformed = true;
return std::nullopt;
}
const auto first_digit = *position + marker.size();
const auto value = parse_hex_at(text, first_digit, malformed);
if (value && !is_field_delimiter(text, first_digit + 4)) {
malformed = true;
return std::nullopt;
}
return value;
}
}
Result<HardwareIdentity> parse_pci_hardware_id(std::string_view text) {
if (!starts_with_ascii_case_insensitive(text, "PCI\\")) {
return Result<HardwareIdentity>{{Error{
ErrorCode::invalid_argument,
"Hardware ID must use the PCI enumerator"}}};
}
bool malformed = false;
const auto vendor_id = parse_hex_field(text, "VEN_", malformed);
const auto device_id = parse_hex_field(text, "DEV_", malformed);
if (malformed || !vendor_id || !device_id) {
return Result<HardwareIdentity>{{Error{
ErrorCode::invalid_argument,
"PCI hardware ID has a missing or malformed vendor/device field"}}};
}
HardwareIdentity identity{};
identity.vendor_id = *vendor_id;
identity.device_id = *device_id;
if (const auto subsystem = find_ascii_case_insensitive(text, "SUBSYS_")) {
const auto first_digit = *subsystem + std::string_view("SUBSYS_").size();
if (*subsystem == 0 ||
(text[*subsystem - 1] != '\\' && text[*subsystem - 1] != '&') ||
first_digit + 8 > text.size() ||
!is_field_delimiter(text, first_digit + 8)) {
return Result<HardwareIdentity>{{Error{
ErrorCode::invalid_argument,
"PCI hardware ID has an invalid subsystem field boundary"}}};
}
bool subsystem_malformed = false;
const auto subsystem_device = parse_hex_at(text, first_digit, subsystem_malformed);
const auto subsystem_vendor = parse_hex_at(text, first_digit + 4, subsystem_malformed);
if (subsystem_malformed || !subsystem_device || !subsystem_vendor) {
return Result<HardwareIdentity>{{Error{
ErrorCode::invalid_argument,
"PCI hardware ID has a malformed subsystem field"}}};
}
identity.subsystem_device_id = *subsystem_device;
identity.subsystem_vendor_id = *subsystem_vendor;
}
return Result<HardwareIdentity>{{identity}};
}
Result<PciBusAddress> parse_windows_pci_address(
std::uint32_t bus_number,
std::uint32_t address) {
const auto device = address >> 16;
const auto function = address & 0xFFFFu;
if (bus_number > 0xFFu || device > 0x1Fu || function > 0x07u) {
return Result<PciBusAddress>{{Error{
ErrorCode::invalid_argument,
"Windows PCI bus/device/function properties exceed PCI limits"}}};
}
return Result<PciBusAddress>{{PciBusAddress{
static_cast<std::uint8_t>(bus_number),
static_cast<std::uint8_t>(device),
static_cast<std::uint8_t>(function)}}};
}
}
