#include "pseries/recovery.hpp"
#include "pseries/device_selection.hpp"
#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
namespace pseries {
namespace {
std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept {
std::uint32_t value = 0xFFFFFFFFu;
for (const auto byte : bytes) {
value ^= byte;
for (unsigned bit = 0; bit < 8; ++bit) {
value = (value >> 1) ^ ((value & 1u) != 0 ? 0xEDB88320u : 0u);
}
}
return ~value;
}
std::uint32_t read_u32_le(const std::uint8_t* bytes) noexcept {
return static_cast<std::uint32_t>(bytes[0]) |
(static_cast<std::uint32_t>(bytes[1]) << 8) |
(static_cast<std::uint32_t>(bytes[2]) << 16) |
(static_cast<std::uint32_t>(bytes[3]) << 24);
}
using ByteIterator = std::vector<std::uint8_t>::iterator;
void append_u8(ByteIterator& out, std::uint8_t value) {
*out++ = value;
}
void append_u16_le(ByteIterator& out, std::uint16_t value) {
append_u8(out, static_cast<std::uint8_t>(value));
append_u8(out, static_cast<std::uint8_t>(value >> 8));
}
void append_u32_le(ByteIterator& out, std::uint32_t value) {
for (unsigned shift = 0; shift < 32; shift += 8) {
append_u8(out, static_cast<std::uint8_t>(value >> shift));
}
}
void append_u64_le(ByteIterator& out, std::uint64_t value) {
for (unsigned shift = 0; shift < 64; shift += 8) {
append_u8(out, static_cast<std::uint8_t>(value >> shift));
}
}
class ByteReader final {
public:
explicit ByteReader(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {
}
bool read_u8(std::uint8_t& value) noexcept {
if (position_ >= bytes_.size()) {
return false;
}
value = bytes_[position_++];
return true;
}
bool read_u16_le(std::uint16_t& value) noexcept {
std::uint8_t low = 0;
std::uint8_t high = 0;
if (!read_u8(low) || !read_u8(high)) {
return false;
}
value = static_cast<std::uint16_t>(low) |
(static_cast<std::uint16_t>(high) << 8);
return true;
}
bool read_u32_le(std::uint32_t& value) noexcept {
value = 0;
for (unsigned shift = 0; shift < 32; shift += 8) {
std::uint8_t byte = 0;
if (!read_u8(byte)) {
return false;
}
value |= static_cast<std::uint32_t>(byte) << shift;
}
return true;
}
bool read_u64_le(std::uint64_t& value) noexcept {
value = 0;
for (unsigned shift = 0; shift < 64; shift += 8) {
std::uint8_t byte = 0;
if (!read_u8(byte)) {
return false;
}
value |= static_cast<std::uint64_t>(byte) << shift;
}
return true;
}
[[nodiscard]] bool empty() const noexcept {
return position_ == bytes_.size();
}
private:
std::span<const std::uint8_t> bytes_;
std::size_t position_{};
};
constexpr std::uint32_t transaction_journal_magic = 0x31534E50u;
constexpr std::uint32_t transaction_journal_version = 6u;
constexpr std::size_t recovery_header_size = 3 * sizeof(std::uint32_t);
constexpr std::size_t journal_checksum_size = sizeof(std::uint32_t);
constexpr std::size_t maximum_journal_snapshots = 32;
bool valid_phase(P104TransactionPhase phase) noexcept {
return phase >= P104TransactionPhase::prepared &&
phase <= P104TransactionPhase::policy_armed;
}
bool valid_write_state(JournalWriteState state) noexcept {
return state >= JournalWriteState::planned &&
state <= JournalWriteState::restored;
}
bool valid_snapshot(const JournalRegisterSnapshot& snapshot) noexcept {
switch (snapshot.kind) {
case JournalRegisterKind::gpu_config:
return (snapshot.offset == 0x04 && snapshot.width == 2) ||
(snapshot.offset == 0x10 && snapshot.width == 4) ||
(snapshot.offset == 0xA8 && snapshot.width == 2);
case JournalRegisterKind::upstream_config:
return (snapshot.offset == 0x3E && snapshot.width == 2) ||
(snapshot.width == 1 &&
snapshot.offset >= 0x40 &&
snapshot.offset <= 0xFF &&
(snapshot.offset & 0x03u) == 0);
case JournalRegisterKind::gpu_bar0:
return snapshot.width == 4 &&
(snapshot.offset == 0x88610 ||
snapshot.offset == 0x8841C ||
snapshot.offset == 0x8872C ||
snapshot.offset == 0x8C040);
}
return false;
}
Error invalid_journal(const char* message) {
return Error{ErrorCode::invalid_recovery_state, message};
}
bool is_next_normal_phase(
P104TransactionPhase current,
P104TransactionPhase next) noexcept {
switch (current) {
case P104TransactionPhase::prepared:
return next == P104TransactionPhase::gpu_disabled ||
next == P104TransactionPhase::policy_armed;
case P104TransactionPhase::policy_armed:
return next == P104TransactionPhase::gpu_disabled;
case P104TransactionPhase::gpu_disabled:
return next == P104TransactionPhase::temporary_access;
case P104TransactionPhase::temporary_access:
return next == P104TransactionPhase::width_configured;
case P104TransactionPhase::width_configured:
return next == P104TransactionPhase::link_retrained;
case P104TransactionPhase::link_retrained:
return next == P104TransactionPhase::gpu_enabled;
case P104TransactionPhase::gpu_enabled:
return next == P104TransactionPhase::speed_configured;
case P104TransactionPhase::speed_configured:
return next == P104TransactionPhase::verified;
case P104TransactionPhase::verified:
return next == P104TransactionPhase::committed;
default:
return false;
}
}
}
Result<void> validate_transaction_journal(
const P104TransactionJournal& journal) noexcept {
const bool same_bdf =
journal.gpu_bdf.domain == journal.upstream_bdf.domain &&
journal.gpu_bdf.bus == journal.upstream_bdf.bus &&
journal.gpu_bdf.device == journal.upstream_bdf.device &&
journal.gpu_bdf.function == journal.upstream_bdf.function;
const bool valid_gpu_identity = is_known_p_series_gpu(
HardwareIdentity{
journal.gpu_vendor_id,
journal.gpu_device_id,
std::nullopt,
std::nullopt,
std::nullopt});
const bool valid_upstream_identity =
journal.upstream_vendor_id != 0 &&
journal.upstream_vendor_id != 0xFFFF &&
journal.upstream_device_id != 0 &&
journal.upstream_device_id != 0xFFFF;
if (journal.transaction_id == 0 ||
!journal.gpu_bdf.is_valid() ||
!journal.upstream_bdf.is_valid() ||
journal.gpu_bdf.domain != journal.upstream_bdf.domain ||
same_bdf ||
journal.upstream_bdf.bus > journal.gpu_bdf.bus ||
!valid_gpu_identity ||
!valid_upstream_identity) {
return Result<void>{{invalid_journal(
"Transaction journal target is not a valid GPU/upstream bridge pair")}};
}
const auto accepted_width = journal.accepted_width_ceiling.lanes;
if ((journal.initial_generation != LinkGeneration::gen1 &&
journal.initial_generation != LinkGeneration::gen2) ||
(journal.initial_width.lanes != 4 &&
journal.initial_width.lanes != 8 &&
journal.initial_width.lanes != 16) ||
(journal.target_generation != LinkGeneration::gen1 &&
journal.target_generation != LinkGeneration::gen2) ||
(accepted_width != 4 && accepted_width != 8 && accepted_width != 16) ||
!valid_phase(journal.phase)) {
return Result<void>{{invalid_journal(
"Transaction journal link state or phase is unsupported")}};
}
if (journal.snapshots.size() > maximum_journal_snapshots) {
return Result<void>{{invalid_journal(
"Transaction journal contains too many register snapshots")}};
}
bool has_planned_write = false;
bool has_pending_write = false;
bool has_applied_write = false;
bool has_restored_write = false;
for (std::size_t index = 0; index < journal.snapshots.size(); ++index) {
const auto& snapshot = journal.snapshots[index];
if (!valid_snapshot(snapshot)) {
return Result<void>{{invalid_journal(
"Transaction journal contains an unreviewed register")}};
}
if (!valid_write_state(snapshot.state)) {
return Result<void>{{invalid_journal(
"Transaction journal contains an invalid register write state")}};
}
has_planned_write = has_planned_write ||
snapshot.state == JournalWriteState::planned;
has_pending_write = has_pending_write ||
snapshot.state == JournalWriteState::pending;
has_applied_write = has_applied_write ||
snapshot.state == JournalWriteState::applied;
has_restored_write = has_restored_write ||
snapshot.state == JournalWriteState::restored;
const std::uint32_t value_mask = snapshot.width == 1
? 0xFFu
: (snapshot.width == 2 ? 0xFFFFu : 0xFFFFFFFFu);
if ((snapshot.before & ~value_mask) != 0 ||
(snapshot.intended & ~value_mask) != 0) {
return Result<void>{{invalid_journal(
"Transaction journal register value exceeds its access width")}};
}
}
if ((journal.phase == P104TransactionPhase::prepared &&
(has_applied_write || has_restored_write)) ||
((journal.phase == P104TransactionPhase::verified ||
journal.phase == P104TransactionPhase::committed) &&
(has_planned_write || has_pending_write)) ||
(journal.phase == P104TransactionPhase::recovered &&
(has_pending_write || has_applied_write))) {
return Result<void>{{invalid_journal(
"Transaction phase conflicts with register write states")}};
}
return Result<void>{};
}
Result<std::vector<std::uint8_t>> serialize_transaction_journal(
const P104TransactionJournal& journal) {
const auto validation = validate_transaction_journal(journal);
if (!validation.has_value()) {
return Result<std::vector<std::uint8_t>>{{validation.error()}};
}
const auto total_size = recovery_header_size + 32 +
journal.snapshots.size() * 15 + journal_checksum_size;
std::vector<std::uint8_t> bytes(total_size, 0);
auto out = bytes.begin();
append_u32_le(out, transaction_journal_magic);
append_u32_le(out, transaction_journal_version);
const auto payload_size = static_cast<std::uint32_t>(
32 + journal.snapshots.size() * 15 + journal_checksum_size);
append_u32_le(out, payload_size + static_cast<std::uint32_t>(recovery_header_size));
append_u64_le(out, journal.transaction_id);
append_u16_le(out, journal.gpu_bdf.domain);
append_u8(out, journal.gpu_bdf.bus);
append_u8(out, journal.gpu_bdf.device);
append_u8(out, journal.gpu_bdf.function);
append_u16_le(out, journal.upstream_bdf.domain);
append_u8(out, journal.upstream_bdf.bus);
append_u8(out, journal.upstream_bdf.device);
append_u8(out, journal.upstream_bdf.function);
append_u16_le(out, journal.gpu_vendor_id);
append_u16_le(out, journal.gpu_device_id);
append_u16_le(out, journal.upstream_vendor_id);
append_u16_le(out, journal.upstream_device_id);
append_u8(out, static_cast<std::uint8_t>(journal.initial_generation));
append_u8(out, journal.initial_width.lanes);
append_u8(out, static_cast<std::uint8_t>(journal.target_generation));
append_u8(out, journal.accepted_width_ceiling.lanes);
append_u8(out, static_cast<std::uint8_t>(journal.phase));
append_u8(out, static_cast<std::uint8_t>(journal.snapshots.size()));
for (const auto& snapshot : journal.snapshots) {
append_u8(out, static_cast<std::uint8_t>(snapshot.kind));
append_u32_le(out, snapshot.offset);
append_u8(out, snapshot.width);
append_u8(out, static_cast<std::uint8_t>(snapshot.state));
append_u32_le(out, snapshot.before);
append_u32_le(out, snapshot.intended);
}
const auto checksum = crc32(std::span<const std::uint8_t>(
bytes.data(), bytes.size() - journal_checksum_size));
append_u32_le(out, checksum);
return Result<std::vector<std::uint8_t>>{std::move(bytes)};
}
Result<P104TransactionJournal> parse_transaction_journal(
std::span<const std::uint8_t> bytes) {
const auto envelope = parse_recovery_state(bytes, {});
if (!envelope.has_value()) {
return Result<P104TransactionJournal>{{envelope.error()}};
}
if (bytes.size() < recovery_header_size + 32 + journal_checksum_size ||
read_u32_le(bytes.data()) != transaction_journal_magic ||
read_u32_le(bytes.data() + sizeof(std::uint32_t)) != transaction_journal_version) {
return Result<P104TransactionJournal>{{invalid_journal(
"Transaction journal header is invalid")}};
}
ByteReader reader(bytes.subspan(
recovery_header_size,
bytes.size() - recovery_header_size - journal_checksum_size));
P104TransactionJournal journal{};
std::uint8_t initial_generation = 0;
std::uint8_t initial_width = 0;
std::uint8_t target_generation = 0;
std::uint8_t accepted_width = 0;
std::uint8_t phase = 0;
std::uint8_t snapshot_count = 0;
if (!reader.read_u64_le(journal.transaction_id) ||
!reader.read_u16_le(journal.gpu_bdf.domain) ||
!reader.read_u8(journal.gpu_bdf.bus) ||
!reader.read_u8(journal.gpu_bdf.device) ||
!reader.read_u8(journal.gpu_bdf.function) ||
!reader.read_u16_le(journal.upstream_bdf.domain) ||
!reader.read_u8(journal.upstream_bdf.bus) ||
!reader.read_u8(journal.upstream_bdf.device) ||
!reader.read_u8(journal.upstream_bdf.function) ||
!reader.read_u16_le(journal.gpu_vendor_id) ||
!reader.read_u16_le(journal.gpu_device_id) ||
!reader.read_u16_le(journal.upstream_vendor_id) ||
!reader.read_u16_le(journal.upstream_device_id) ||
!reader.read_u8(initial_generation) ||
!reader.read_u8(initial_width) ||
!reader.read_u8(target_generation) ||
!reader.read_u8(accepted_width) ||
!reader.read_u8(phase) ||
!reader.read_u8(snapshot_count) ||
snapshot_count > maximum_journal_snapshots) {
return Result<P104TransactionJournal>{{invalid_journal(
"Transaction journal payload is truncated or invalid")}};
}
journal.initial_generation = static_cast<LinkGeneration>(initial_generation);
journal.initial_width = LinkWidth{initial_width};
journal.target_generation = static_cast<LinkGeneration>(target_generation);
journal.accepted_width_ceiling = LinkWidth{accepted_width};
journal.phase = static_cast<P104TransactionPhase>(phase);
journal.snapshots.reserve(snapshot_count);
for (std::uint8_t index = 0; index < snapshot_count; ++index) {
JournalRegisterSnapshot snapshot{};
std::uint8_t kind = 0;
std::uint8_t write_state = 0;
if (!reader.read_u8(kind) ||
!reader.read_u32_le(snapshot.offset) ||
!reader.read_u8(snapshot.width) ||
!reader.read_u8(write_state) ||
!reader.read_u32_le(snapshot.before) ||
!reader.read_u32_le(snapshot.intended)) {
return Result<P104TransactionJournal>{{invalid_journal(
"Transaction journal register list is truncated")}};
}
snapshot.kind = static_cast<JournalRegisterKind>(kind);
snapshot.state = static_cast<JournalWriteState>(write_state);
journal.snapshots.push_back(snapshot);
}
if (!reader.empty()) {
return Result<P104TransactionJournal>{{invalid_journal(
"Transaction journal contains trailing payload data")}};
}
const auto validation = validate_transaction_journal(journal);
if (!validation.has_value()) {
return Result<P104TransactionJournal>{{validation.error()}};
}
if (std::any_of(
journal.snapshots.begin(),
journal.snapshots.end(),
[](const JournalRegisterSnapshot& snapshot) {
return snapshot.state == JournalWriteState::pending;
})) {
if (journal.phase == P104TransactionPhase::committed ||
journal.phase == P104TransactionPhase::recovered) {
return Result<P104TransactionJournal>{{invalid_journal(
"Terminal transaction contains an ambiguous register write")}};
}
journal.phase = P104TransactionPhase::recovery_required;
}
return Result<P104TransactionJournal>{std::move(journal)};
}
Result<P104TransactionPhase> advance_transaction_phase(
P104TransactionPhase current,
P104TransactionPhase next) noexcept {
if (!valid_phase(current) || !valid_phase(next)) {
return Result<P104TransactionPhase>{{invalid_journal(
"Transaction phase value is invalid")}};
}
if (is_next_normal_phase(current, next)) {
return Result<P104TransactionPhase>{next};
}
if (next == P104TransactionPhase::recovery_required &&
current != P104TransactionPhase::committed &&
current != P104TransactionPhase::recovered &&
current != P104TransactionPhase::blocked &&
current != P104TransactionPhase::recovery_required &&
current != P104TransactionPhase::recovering) {
return Result<P104TransactionPhase>{next};
}
if (current == P104TransactionPhase::recovery_required &&
next == P104TransactionPhase::recovering) {
return Result<P104TransactionPhase>{next};
}
if (current == P104TransactionPhase::recovering &&
(next == P104TransactionPhase::recovered ||
next == P104TransactionPhase::blocked)) {
return Result<P104TransactionPhase>{next};
}
return Result<P104TransactionPhase>{{invalid_journal(
"Transaction phase transition is not permitted")}};
}
Result<JournalWriteState> advance_journal_write_state(
JournalWriteState current,
JournalWriteState next) noexcept {
if (!valid_write_state(current) || !valid_write_state(next)) {
return Result<JournalWriteState>{{invalid_journal(
"Register write state value is invalid")}};
}
if ((current == JournalWriteState::planned &&
next == JournalWriteState::pending) ||
(current == JournalWriteState::pending &&
(next == JournalWriteState::applied ||
next == JournalWriteState::restored)) ||
(current == JournalWriteState::applied &&
next == JournalWriteState::restored)) {
return Result<JournalWriteState>{next};
}
return Result<JournalWriteState>{{invalid_journal(
"Register write state transition is not permitted")}};
}
Result<void> apply_journaled_write(
P104TransactionJournal& journal,
std::size_t snapshot_index,
const PersistP104TransactionJournal& persist,
const ReadJournaledRegister& read_current,
const ApplyAndVerifyJournaledWrite& apply_and_verify) {
const auto validation = validate_transaction_journal(journal);
if (!validation.has_value()) {
return validation;
}
if (!persist || !read_current || !apply_and_verify ||
snapshot_index >= journal.snapshots.size()) {
return Result<void>{{Error{
ErrorCode::invalid_argument,
"Journaled write requires valid callbacks and a register snapshot"}}};
}
if (journal.phase != P104TransactionPhase::policy_armed &&
journal.phase != P104TransactionPhase::temporary_access &&
journal.phase != P104TransactionPhase::width_configured &&
journal.phase != P104TransactionPhase::link_retrained &&
journal.phase != P104TransactionPhase::gpu_enabled &&
journal.phase != P104TransactionPhase::speed_configured) {
return Result<void>{{invalid_journal(
"Journaled write is not permitted in the current transaction phase")}};
}
auto& snapshot = journal.snapshots[snapshot_index];
if (snapshot.state != JournalWriteState::planned) {
return Result<void>{{invalid_journal(
"Register snapshot is not in the planned state")}};
}
const auto refuse_write = [&journal, &persist](Error error) {
journal.phase = P104TransactionPhase::recovery_required;
const auto recovery_saved = persist(journal);
if (!recovery_saved.has_value()) {
error.message += "; failed to persist recovery-required journal: ";
error.message += recovery_saved.error().message;
}
return Result<void>{{std::move(error)}};
};
const auto current = read_current(snapshot);
if (!current.has_value()) {
return refuse_write(current.error());
}
if (current.get() != snapshot.before) {
return refuse_write(Error{
ErrorCode::invalid_recovery_state,
"Register changed since snapshot; expected " +
std::to_string(snapshot.before) + ", observed " +
std::to_string(current.get())});
}
snapshot.state = JournalWriteState::pending;
const auto pending_saved = persist(journal);
if (!pending_saved.has_value()) {
snapshot.state = JournalWriteState::planned;
return pending_saved;
}
const auto applied = apply_and_verify(snapshot);
if (!applied.has_value()) {
journal.phase = P104TransactionPhase::recovery_required;
const auto recovery_saved = persist(journal);
std::string message = applied.error().message;
if (!recovery_saved.has_value()) {
message += "; failed to persist recovery-required journal: ";
message += recovery_saved.error().message;
}
return Result<void>{{Error{applied.error().code, std::move(message)}}};
}
snapshot.state = JournalWriteState::applied;
const auto applied_saved = persist(journal);
if (applied_saved.has_value()) {
return Result<void>{};
}
journal.phase = P104TransactionPhase::recovery_required;
snapshot.state = JournalWriteState::pending;
const auto recovery_saved = persist(journal);
std::string message = applied_saved.error().message;
if (!recovery_saved.has_value()) {
message += "; failed to persist recovery-required journal: ";
message += recovery_saved.error().message;
}
return Result<void>{{Error{applied_saved.error().code, std::move(message)}}};
}
Result<RecoveryStateStatus> parse_recovery_state(
std::span<const std::uint8_t> bytes,
const RecoveryValidationOptions& options) noexcept {
if (bytes.empty()) {
return Result<RecoveryStateStatus>{{RecoveryStateStatus::none}};
}
if (bytes.size() < sizeof(RecoveryRecordHeader)) {
return Result<RecoveryStateStatus>{{Error{
ErrorCode::invalid_recovery_state,
"Recovery record is shorter than its header"}}};
}
RecoveryRecordHeader header{};
std::memcpy(&header, bytes.data(), sizeof(header));
if (header.magic != 0x31534e50u) {
return Result<RecoveryStateStatus>{{Error{
ErrorCode::invalid_recovery_state,
"Recovery record magic mismatch"}}};
}
if (header.version < 1u || header.version > 6u) {
return Result<RecoveryStateStatus>{{Error{
ErrorCode::invalid_recovery_state,
"Unsupported recovery schema"}}};
}
if (header.payload_size != bytes.size()) {
return Result<RecoveryStateStatus>{{Error{
ErrorCode::invalid_recovery_state,
"Recovery payload size mismatch"}}};
}
if (header.version == 6u) {
constexpr auto checksum_size = sizeof(std::uint32_t);
if (bytes.size() < sizeof(RecoveryRecordHeader) + checksum_size) {
return Result<RecoveryStateStatus>{{Error{
ErrorCode::invalid_recovery_state,
"Version 6 recovery record is missing its checksum"}}};
}
const auto checksum_offset = bytes.size() - checksum_size;
const auto expected_checksum = read_u32_le(bytes.data() + checksum_offset);
if (crc32(bytes.first(checksum_offset)) != expected_checksum) {
return Result<RecoveryStateStatus>{{Error{
ErrorCode::invalid_recovery_state,
"Version 6 recovery checksum mismatch"}}};
}
}
if (options.allow_unknown_fields) {
}
return Result<RecoveryStateStatus>{{RecoveryStateStatus::pending}};
}
}
