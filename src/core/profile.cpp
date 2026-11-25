#include "pseries/profile.hpp"
#include <string>
namespace pseries {
const char* p104_profile_name(P104Profile profile) noexcept {
switch (profile) {
case P104Profile::gen1_4x:
return "4x1";
case P104Profile::gen1_8x:
return "8x1";
case P104Profile::gen1_16x:
return "16x1";
case P104Profile::gen2_4x:
return "4x2";
case P104Profile::gen2_8x:
return "8x2";
case P104Profile::gen2_16x:
return "16x2";
}
return "unknown";
}
Result<P104Profile> parse_p104_profile(std::string_view name) noexcept {
if (name == "4x1" || name == "4x1.0" || name == "4x1.1" ||
name == "4x1,0" || name == "4x1,1") {
return Result<P104Profile>{P104Profile::gen1_4x};
}
if (name == "8x1" || name == "8x1.0" || name == "8x1.1" ||
name == "8x1,0" || name == "8x1,1") {
return Result<P104Profile>{P104Profile::gen1_8x};
}
if (name == "16x1" || name == "16x1.0" || name == "16x1.1" ||
name == "16x1,0" || name == "16x1,1") {
return Result<P104Profile>{P104Profile::gen1_16x};
}
if (name == "4x2" || name == "4x2.0" || name == "4x2,0") {
return Result<P104Profile>{P104Profile::gen2_4x};
}
if (name == "8x2" || name == "8x2.0" || name == "8x2,0") {
return Result<P104Profile>{P104Profile::gen2_8x};
}
if (name == "16x2" || name == "16x2.0" || name == "16x2,0") {
return Result<P104Profile>{P104Profile::gen2_16x};
}
return Result<P104Profile>{{Error{
ErrorCode::invalid_configuration,
"Profile must be 4|8|16 lanes at x1 or x2"}}};
}
Result<std::uint8_t> p104_profile_lanes(P104Profile profile) noexcept {
switch (profile) {
case P104Profile::gen1_4x:
case P104Profile::gen2_4x:
return Result<std::uint8_t>{std::uint8_t{4}};
case P104Profile::gen1_8x:
case P104Profile::gen2_8x:
return Result<std::uint8_t>{std::uint8_t{8}};
case P104Profile::gen1_16x:
case P104Profile::gen2_16x:
return Result<std::uint8_t>{std::uint8_t{16}};
}
return Result<std::uint8_t>{{Error{
ErrorCode::invalid_configuration,
"Unknown P104 unlock profile"}}};
}
Result<LinkGeneration> p104_profile_generation(P104Profile profile) noexcept {
switch (profile) {
case P104Profile::gen1_4x:
case P104Profile::gen1_8x:
case P104Profile::gen1_16x:
return Result<LinkGeneration>{LinkGeneration::gen1};
case P104Profile::gen2_4x:
case P104Profile::gen2_8x:
case P104Profile::gen2_16x:
return Result<LinkGeneration>{LinkGeneration::gen2};
}
return Result<LinkGeneration>{{Error{
ErrorCode::invalid_configuration,
"Unknown P104 unlock profile"}}};
}
Result<P104Profile> make_p104_profile(
LinkGeneration generation,
std::uint8_t lanes) noexcept {
if (generation == LinkGeneration::gen1) {
switch (lanes) {
case 4:
return Result<P104Profile>{P104Profile::gen1_4x};
case 8:
return Result<P104Profile>{P104Profile::gen1_8x};
case 16:
return Result<P104Profile>{P104Profile::gen1_16x};
default:
break;
}
}
if (generation == LinkGeneration::gen2) {
switch (lanes) {
case 4:
return Result<P104Profile>{P104Profile::gen2_4x};
case 8:
return Result<P104Profile>{P104Profile::gen2_8x};
case 16:
return Result<P104Profile>{P104Profile::gen2_16x};
default:
break;
}
}
return Result<P104Profile>{{Error{
ErrorCode::invalid_configuration,
"Width must be 4, 8, or 16 lanes at Gen1 or Gen2"}}};
}
Result<P104ServiceProfile> parse_p104_service_profile(
std::string_view text) noexcept {
P104ServiceProfile result{};
bool schema_seen = false;
bool profile_seen = false;
bool decorative_seen = false;
while (!text.empty()) {
const std::size_t newline = text.find('\n');
std::string_view line = text.substr(0, newline);
if (newline == std::string_view::npos) {
text = {};
} else {
text.remove_prefix(newline + 1);
}
while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
line.remove_suffix(1);
}
if (line.empty() || line.front() == '#') {
continue;
}
const std::size_t equals = line.find('=');
if (equals == std::string_view::npos) {
return Result<P104ServiceProfile>{{Error{
ErrorCode::invalid_configuration,
"Profile line has no '='"}}};
}
const std::string_view key = line.substr(0, equals);
const std::string_view value = line.substr(equals + 1);
if (key == "schema_version") {
schema_seen = true;
if (value != "2") {
return Result<P104ServiceProfile>{{Error{
ErrorCode::invalid_configuration,
"Unsupported profile schema"}}};
}
result.schema_version = 2;
} else if (key == "profile") {
profile_seen = true;
const auto parsed_profile = parse_p104_profile(value);
if (!parsed_profile.has_value()) {
return Result<P104ServiceProfile>{{parsed_profile.error()}};
}
result.profile = parsed_profile.get();
} else if (key == "decorative_x16") {
decorative_seen = true;
if (value == "true") {
result.decorative_x16 = true;
} else if (value == "false") {
result.decorative_x16 = false;
} else {
return Result<P104ServiceProfile>{{Error{
ErrorCode::invalid_configuration,
"decorative_x16 must be true or false"}}};
}
} else if (key == "driver_path") {
result.driver_path = std::string(value);
} else {
return Result<P104ServiceProfile>{{Error{
ErrorCode::invalid_configuration,
"Unknown profile configuration key"}}};
}
}
if (!schema_seen || !profile_seen || !decorative_seen) {
return Result<P104ServiceProfile>{{Error{
ErrorCode::invalid_configuration,
"Required profile values are missing"}}};
}
if (!result.decorative_x16) {
return Result<P104ServiceProfile>{{Error{
ErrorCode::invalid_configuration,
"This build always declares x16; decorative_x16 must be true"}}};
}
return Result<P104ServiceProfile>{result};
}
Result<std::string> serialize_p104_service_profile(
const P104ServiceProfile& profile) {
if (profile.schema_version != 2 || !profile.decorative_x16) {
return Result<std::string>{{Error{
ErrorCode::invalid_configuration,
"Refusing to serialize an unsupported profile"}}};
}
const auto lanes = p104_profile_lanes(profile.profile);
if (!lanes.has_value()) {
return Result<std::string>{{lanes.error()}};
}
std::string text;
text += "schema_version=2\n";
text += "profile=";
text += p104_profile_name(profile.profile);
text += "\n";
text += "decorative_x16=true\n";
text += "driver_path=";
text += profile.driver_path;
text += "\n";
return Result<std::string>{std::move(text)};
}
}
