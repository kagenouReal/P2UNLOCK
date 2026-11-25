#pragma once
#include "pseries/recovery.hpp"
#include "pseries/types.hpp"
#include <cstdint>
#include <vector>
namespace pseries {
struct P104InitialRegisters {
std::uint32_t upstream_3e{};
std::uint32_t upstream_link_control{};
std::uint32_t upstream_link_control_2{};
std::uint16_t upstream_link_control_offset{};
std::uint16_t upstream_link_control_2_offset{};
std::uint32_t gpu_04{};
std::uint32_t gpu_10{};
std::uint32_t gpu_a8{};
std::uint32_t bar0_88610{};
std::uint32_t bar0_8841c{};
std::uint32_t bar0_8872c{};
std::uint32_t bar0_8c040{};
};
struct P104SpeedRegisters {
std::uint32_t upstream_link_control_2{};
std::uint16_t upstream_link_control_2_offset{};
std::uint32_t gpu_a8{};
std::uint32_t bar0_88610{};
std::uint32_t bar0_8841c{};
std::uint32_t bar0_8872c{};
std::uint32_t bar0_8c040{};
};
struct P104TargetProfile {
std::uint8_t lanes{};
std::uint32_t policy_clear_mask{};
std::uint32_t policy_high_value{};
std::uint32_t policy_aux_or{};
std::uint32_t width_value{};
std::uint32_t width_finish_value{};
bool has_width_phase{false};
bool has_width_finish{false};
std::uint32_t speed_pre_commit{};
std::uint32_t speed_commit{};
};
struct P104TargetSelection {
P104TargetProfile request;
std::uint8_t accepted_lanes{};
};
[[nodiscard]] Result<P104TargetSelection> resolve_p104_target_selection(
std::uint8_t requested_lanes,
std::uint8_t gpu_maximum_lanes,
std::uint8_t upstream_maximum_lanes);
[[nodiscard]] Result<std::vector<JournalRegisterSnapshot>> build_p104_policy_and_width_plan(
const P104InitialRegisters& registers,
std::uint8_t target_lanes,
LinkGeneration target_generation = LinkGeneration::gen2,
bool include_width_phase = true);
[[nodiscard]] Result<std::vector<JournalRegisterSnapshot>> build_p104_speed_plan(
const P104SpeedRegisters& registers,
std::uint8_t target_lanes,
LinkGeneration target_generation = LinkGeneration::gen2);
[[nodiscard]] Result<std::uint32_t> build_p104_gen1_policy_value(
std::uint8_t lanes);
constexpr std::uint32_t kP104LinkConfig0Offset = 0x8C040u;
constexpr std::uint32_t kP104LinkConfigWidthMask = 0xF8000000u;
constexpr std::uint32_t kP104DecorativeX16Value = 0x80000000u;
constexpr std::uint32_t kP104LinkConfigWidthBits = 0xE0000000u;
constexpr std::uint32_t kP104LinkConfigBit29 = 0x20000000u;
constexpr std::uint32_t kP104LinkConfigBit30 = 0x40000000u;
[[nodiscard]] bool is_p104_link_config_value_legal(std::uint32_t value) noexcept;
[[nodiscard]] Result<std::uint32_t> build_p104_declaration_value(
std::uint32_t current,
std::uint8_t lanes);
[[nodiscard]] Result<std::uint32_t> build_p104_decorative_x16_value(
std::uint32_t current);
}
