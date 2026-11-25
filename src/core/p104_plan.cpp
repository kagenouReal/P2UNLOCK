#include "pseries/p104_plan.hpp"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>
namespace pseries {
namespace {
constexpr std::uint32_t kGen1BaselineLowBits = 0x00585800u;
constexpr std::uint32_t kSpeedHighMask = 0xE0000000u;
constexpr std::uint32_t kUpstreamLinkControlRelevantMask = 0x50u;
constexpr std::uint32_t kUpstreamLinkControlExpectedBits = 0x40u;
constexpr std::uint32_t kUpstreamLinkControl2SpeedMask = 0x0Fu;
constexpr std::uint32_t kGen1UpstreamLinkControl2Speed = 0x03u;
constexpr std::uint32_t kGen2UpstreamLinkControl2Speed = 0x02u;
bool valid_upstream_capability_offset(std::uint16_t offset) noexcept {
return offset != 0 && offset <= 0xFF && (offset & 0x03u) == 0;
}
std::optional<P104TargetProfile> known_profile(std::uint8_t lanes) {
switch (lanes) {
case 4:
return P104TargetProfile{
4,
0x00080000u,
0x20000000u,
0x00040000u,
0x20089000u,
0x20085800u,
true,
true,
0x20545800u,
0x20545801u};
case 8:
return P104TargetProfile{
8,
0x00580000u,
0x40000000u,
0x00080000u,
0x40089000u,
0u,
true,
false,
0x40445800u,
0x40445801u};
case 16:
return P104TargetProfile{
16,
0x00580000u,
0x80000000u,
0x00080000u,
0x80089000u,
0x80085800u,
true,
true,
0x80485800u,
0x80485801u};
default:
return std::nullopt;
}
}
JournalRegisterSnapshot snapshot(
JournalRegisterKind kind,
std::uint32_t offset,
std::uint8_t width,
std::uint32_t before,
std::uint32_t intended) {
const std::uint32_t mask = width == 1
? 0xFFu
: (width == 2 ? 0xFFFFu : 0xFFFFFFFFu);
return JournalRegisterSnapshot{
kind,
offset,
width,
JournalWriteState::planned,
before & mask,
intended & mask};
}
Error unsafe_state(std::string message) {
return Error{ErrorCode::invalid_recovery_state, std::move(message)};
}
}
Result<P104TargetSelection> resolve_p104_target_selection(
std::uint8_t requested_lanes,
std::uint8_t gpu_maximum_lanes,
std::uint8_t upstream_maximum_lanes) {
if (gpu_maximum_lanes == 0 || upstream_maximum_lanes == 0) {
return Result<P104TargetSelection>{{unsafe_state(
"GPU or upstream maximum link width is unavailable")}};
}
const auto ceiling = std::min(gpu_maximum_lanes, upstream_maximum_lanes);
const auto lanes = requested_lanes == 0 ? ceiling : requested_lanes;
const auto profile = known_profile(lanes);
if (!profile.has_value()) {
return Result<P104TargetSelection>{{unsafe_state(
"P104 unlock supports target widths x4, x8, and x16 only")}};
}
const auto accepted_lanes = static_cast<std::uint8_t>(std::min(lanes, ceiling));
return Result<P104TargetSelection>{P104TargetSelection{*profile, accepted_lanes}};
}
Result<std::vector<JournalRegisterSnapshot>> build_p104_policy_and_width_plan(
const P104InitialRegisters& registers,
std::uint8_t target_lanes,
LinkGeneration target_generation,
bool include_width_phase) {
const auto profile = known_profile(target_lanes);
if (!profile.has_value()) {
return Result<std::vector<JournalRegisterSnapshot>>{{unsafe_state(
"P104 unlock supports target widths x4, x8, and x16 only")}};
}
if (!is_p104_link_config_value_legal(registers.bar0_8c040)) {
return Result<std::vector<JournalRegisterSnapshot>>{{unsafe_state(
"P104 link policy register has an unsafe width-field combination")}};
}
if (target_generation != LinkGeneration::gen1 &&
target_generation != LinkGeneration::gen2) {
return Result<std::vector<JournalRegisterSnapshot>>{{unsafe_state(
"P104 unlock targets Gen1 or Gen2 only")}};
}
if (!valid_upstream_capability_offset(
registers.upstream_link_control_offset) ||
!valid_upstream_capability_offset(
registers.upstream_link_control_2_offset)) {
return Result<std::vector<JournalRegisterSnapshot>>{{unsafe_state(
"Upstream PCIe capability offsets are outside conventional config space")}};
}
if ((registers.upstream_link_control & kUpstreamLinkControlRelevantMask) !=
kUpstreamLinkControlExpectedBits) {
return Result<std::vector<JournalRegisterSnapshot>>{{unsafe_state(
"Upstream link control is not in the expected enabled state")}};
}
const std::uint32_t link_control_preserved =
registers.upstream_link_control & ~kUpstreamLinkControlRelevantMask;
const std::uint32_t link_control_compliance =
link_control_preserved | kUpstreamLinkControlRelevantMask;
const std::uint32_t link_control_restore =
link_control_preserved | kUpstreamLinkControlExpectedBits;
std::uint32_t policy_intent = 0;
if (target_generation == LinkGeneration::gen1) {
const auto restored = build_p104_gen1_policy_value(target_lanes);
if (!restored.has_value()) {
return Result<std::vector<JournalRegisterSnapshot>>{{restored.error()}};
}
policy_intent = restored.get();
} else {
policy_intent =
(registers.bar0_8c040 &
~(kP104LinkConfigWidthMask | profile->policy_clear_mask)) |
profile->policy_high_value |
profile->policy_aux_or;
}
std::vector<JournalRegisterSnapshot> plan;
plan.reserve(
include_width_phase && profile->has_width_phase
? (profile->has_width_finish ? 11 : 10)
: 1);
plan.push_back(snapshot(
JournalRegisterKind::gpu_bar0,
0x8C040,
4,
registers.bar0_8c040,
policy_intent));
if (!include_width_phase || !profile->has_width_phase) {
return Result<std::vector<JournalRegisterSnapshot>>{std::move(plan)};
}
plan.push_back(snapshot(
JournalRegisterKind::upstream_config,
0x3E,
2,
registers.upstream_3e,
0x0040));
plan.push_back(snapshot(
JournalRegisterKind::upstream_config,
0x3E,
2,
0x0040,
0x0000));
plan.push_back(snapshot(
JournalRegisterKind::gpu_config,
0x10,
4,
registers.gpu_10,
registers.gpu_10));
plan.push_back(snapshot(
JournalRegisterKind::gpu_config,
0x04,
2,
registers.gpu_04,
0x0002));
plan.push_back(snapshot(
JournalRegisterKind::gpu_bar0,
0x8C040,
4,
policy_intent,
profile->width_value));
if (profile->has_width_finish) {
plan.push_back(snapshot(
JournalRegisterKind::gpu_bar0,
0x8C040,
4,
profile->width_value,
profile->width_finish_value));
}
plan.push_back(snapshot(
JournalRegisterKind::gpu_config,
0x04,
2,
0x0002,
0x0000));
plan.push_back(snapshot(
JournalRegisterKind::upstream_config,
registers.upstream_link_control_offset,
1,
registers.upstream_link_control,
link_control_compliance));
plan.push_back(snapshot(
JournalRegisterKind::upstream_config,
registers.upstream_link_control_offset,
1,
link_control_compliance,
link_control_restore));
plan.push_back(snapshot(
JournalRegisterKind::gpu_config,
0x10,
4,
registers.gpu_10,
0x00000000));
return Result<std::vector<JournalRegisterSnapshot>>{std::move(plan)};
}
Result<std::vector<JournalRegisterSnapshot>> build_p104_speed_plan(
const P104SpeedRegisters& registers,
std::uint8_t target_lanes,
LinkGeneration target_generation) {
const auto profile = known_profile(target_lanes);
if (!profile.has_value()) {
return Result<std::vector<JournalRegisterSnapshot>>{{unsafe_state(
"P104 unlock supports target widths x4, x8, and x16 only")}};
}
if (!is_p104_link_config_value_legal(registers.bar0_8c040)) {
return Result<std::vector<JournalRegisterSnapshot>>{{unsafe_state(
"P104 width phase left an unsafe 8c040 value (8c040=" +
std::to_string(registers.bar0_8c040) + ")")}};
}
if (!valid_upstream_capability_offset(
registers.upstream_link_control_2_offset)) {
return Result<std::vector<JournalRegisterSnapshot>>{{unsafe_state(
"Upstream Link Control 2 offset is outside conventional config space")}};
}
std::uint32_t pre_commit = 0;
std::uint32_t commit = 0;
std::uint32_t gpu_a8_intended = 0x0002;
std::uint32_t upstream_speed_bits = kGen2UpstreamLinkControl2Speed;
std::uint32_t bar0_88610 = 0x00000001;
std::uint32_t bar0_8841c = 0xC0B42D00;
std::uint32_t bar0_8872c = 0x00000006;
if (target_generation == LinkGeneration::gen1) {
const auto restored = build_p104_gen1_policy_value(target_lanes);
if (!restored.has_value()) {
return Result<std::vector<JournalRegisterSnapshot>>{{restored.error()}};
}
pre_commit = restored.get() | 0x1u;
commit = restored.get() | 0x1u;
gpu_a8_intended = 0x0001;
upstream_speed_bits = kGen1UpstreamLinkControl2Speed;
bar0_88610 = 0x00000000;
bar0_8841c = 0xC0B40500;
bar0_8872c = 0x00000000;
} else if (target_generation == LinkGeneration::gen2) {
if ((registers.bar0_8c040 & kSpeedHighMask) != profile->policy_high_value) {
return Result<std::vector<JournalRegisterSnapshot>>{{unsafe_state(
"P104 width phase did not leave the expected armed policy state (8c040=" +
std::to_string(registers.bar0_8c040) + ")")}};
}
pre_commit = profile->speed_pre_commit;
commit = profile->speed_commit;
} else {
return Result<std::vector<JournalRegisterSnapshot>>{{unsafe_state(
"P104 unlock targets Gen1 or Gen2 only")}};
}
const std::uint32_t upstream_link_control_2 =
(registers.upstream_link_control_2 & ~kUpstreamLinkControl2SpeedMask) |
upstream_speed_bits;
std::vector<JournalRegisterSnapshot> plan;
plan.reserve(8);
plan.push_back(snapshot(
JournalRegisterKind::gpu_bar0,
0x88610,
4,
registers.bar0_88610,
bar0_88610));
plan.push_back(snapshot(
JournalRegisterKind::gpu_bar0,
0x8841C,
4,
registers.bar0_8841c,
bar0_8841c));
plan.push_back(snapshot(
JournalRegisterKind::gpu_bar0,
0x8872C,
4,
registers.bar0_8872c,
bar0_8872c));
plan.push_back(snapshot(
JournalRegisterKind::gpu_bar0,
0x8C040,
4,
registers.bar0_8c040,
pre_commit));
plan.push_back(snapshot(
JournalRegisterKind::gpu_config,
0xA8,
2,
registers.gpu_a8,
gpu_a8_intended));
plan.push_back(snapshot(
JournalRegisterKind::upstream_config,
registers.upstream_link_control_2_offset,
1,
registers.upstream_link_control_2,
upstream_link_control_2));
plan.push_back(snapshot(
JournalRegisterKind::gpu_bar0,
0x8C040,
4,
pre_commit,
commit));
plan.push_back(snapshot(
JournalRegisterKind::gpu_config,
0xA8,
2,
gpu_a8_intended,
gpu_a8_intended));
return Result<std::vector<JournalRegisterSnapshot>>{std::move(plan)};
}
Result<std::uint32_t> build_p104_gen1_policy_value(std::uint8_t lanes) {
std::uint32_t width_value = 0;
switch (lanes) {
case 4:
width_value = kP104LinkConfigBit29;
break;
case 8:
width_value = kP104LinkConfigBit30;
break;
case 16:
width_value = kP104DecorativeX16Value;
break;
default:
return Result<std::uint32_t>{{Error{
ErrorCode::invalid_configuration,
"Gen1 policy width must be 4, 8, or 16 lanes"}}};
}
const auto value = width_value | kGen1BaselineLowBits;
if (!is_p104_link_config_value_legal(value)) {
return Result<std::uint32_t>{{Error{
ErrorCode::invalid_configuration,
"Gen1 policy value has an unsafe width-field combination"}}};
}
return Result<std::uint32_t>{value};
}
bool is_p104_link_config_value_legal(std::uint32_t value) noexcept {
const auto width_bits = value & kP104LinkConfigWidthBits;
return width_bits == kP104LinkConfigBit29 ||
width_bits == kP104LinkConfigBit30 ||
width_bits == kP104DecorativeX16Value;
}
Result<std::uint32_t> build_p104_declaration_value(
std::uint32_t current,
std::uint8_t lanes) {
if (!is_p104_link_config_value_legal(current)) {
return Result<std::uint32_t>{{Error{
ErrorCode::invalid_recovery_state,
"Current 0x8C040 value has an unsafe width-field combination"}}};
}
std::uint32_t width_value = 0;
switch (lanes) {
case 4:
width_value = kP104LinkConfigBit29;
break;
case 8:
width_value = kP104LinkConfigBit30;
break;
case 16:
width_value = kP104DecorativeX16Value;
break;
default:
return Result<std::uint32_t>{{Error{
ErrorCode::invalid_configuration,
"Declaration width must be 4, 8, or 16 lanes"}}};
}
const std::uint32_t intended =
(current & ~kP104LinkConfigWidthMask) | width_value;
if (!is_p104_link_config_value_legal(intended)) {
return Result<std::uint32_t>{{Error{
ErrorCode::invalid_configuration,
"Declaration value has an unsafe width-field combination"}}};
}
return Result<std::uint32_t>{intended};
}
Result<std::uint32_t> build_p104_decorative_x16_value(std::uint32_t current) {
return build_p104_declaration_value(current, 16);
}
}
