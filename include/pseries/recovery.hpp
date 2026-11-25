#pragma once
#include "pseries/device_selection.hpp"
#include "pseries/types.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace pseries {
enum class RecoveryStateStatus : std::uint8_t {
none,
pending,
recovering,
recovered,
blocked,
};
struct RecoveryRecordHeader {
std::uint32_t magic{};
std::uint32_t version{};
std::uint32_t payload_size{};
};
struct RecoveryValidationOptions {
bool allow_unknown_fields{false};
};
enum class JournalRegisterKind : std::uint8_t {
gpu_config = 1,
upstream_config = 2,
gpu_bar0 = 3,
};
enum class JournalWriteState : std::uint8_t {
planned = 1,
pending,
applied,
restored,
};
struct JournalRegisterSnapshot {
JournalRegisterKind kind{JournalRegisterKind::gpu_config};
std::uint32_t offset{};
std::uint8_t width{};
JournalWriteState state{JournalWriteState::planned};
std::uint32_t before{};
std::uint32_t intended{};
};
enum class P104TransactionPhase : std::uint8_t {
prepared = 1,
gpu_disabled,
temporary_access,
width_configured,
link_retrained,
gpu_enabled,
speed_configured,
verified,
committed,
recovery_required,
recovering,
recovered,
blocked,
policy_armed,
};
struct P104TransactionJournal {
std::uint64_t transaction_id{};
PciBdf gpu_bdf{0, 1, 0, 0};
PciBdf upstream_bdf{0, 0, 1, 0};
std::uint16_t gpu_vendor_id{nvidia_vendor_id};
std::uint16_t gpu_device_id{p104_100_device_id};
std::uint16_t upstream_vendor_id{0x8086};
std::uint16_t upstream_device_id{0x0151};
LinkGeneration initial_generation{LinkGeneration::gen1};
LinkWidth initial_width{4};
LinkGeneration target_generation{LinkGeneration::gen2};
LinkWidth accepted_width_ceiling{8};
P104TransactionPhase phase{P104TransactionPhase::prepared};
std::vector<JournalRegisterSnapshot> snapshots;
};
using PersistP104TransactionJournal =
std::function<Result<void>(const P104TransactionJournal&)>;
using ReadJournaledRegister =
std::function<Result<std::uint32_t>(const JournalRegisterSnapshot&)>;
using ApplyAndVerifyJournaledWrite =
std::function<Result<void>(const JournalRegisterSnapshot&)>;
[[nodiscard]] Result<void> validate_transaction_journal(
const P104TransactionJournal& journal) noexcept;
[[nodiscard]] Result<std::vector<std::uint8_t>> serialize_transaction_journal(
const P104TransactionJournal& journal);
[[nodiscard]] Result<P104TransactionJournal> parse_transaction_journal(
std::span<const std::uint8_t> bytes);
[[nodiscard]] Result<P104TransactionPhase> advance_transaction_phase(
P104TransactionPhase current,
P104TransactionPhase next) noexcept;
[[nodiscard]] Result<JournalWriteState> advance_journal_write_state(
JournalWriteState current,
JournalWriteState next) noexcept;
[[nodiscard]] Result<void> apply_journaled_write(
P104TransactionJournal& journal,
std::size_t snapshot_index,
const PersistP104TransactionJournal& persist,
const ReadJournaledRegister& read_current,
const ApplyAndVerifyJournaledWrite& apply_and_verify);
Result<RecoveryStateStatus> parse_recovery_state(
std::span<const std::uint8_t> bytes,
const RecoveryValidationOptions& options) noexcept;
}
