#include "pseries/platform/p104_executor.hpp"
#include "pseries/device_selection.hpp"
#include "pseries/p104_plan.hpp"
#include "pseries/platform/pci_config.hpp"
#include "pseries/platform/pnp_device.hpp"
#include "pseries/platform/seavo_backend.hpp"
#include "pseries/platform/transaction_journal_store.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
namespace pseries::platform {
namespace {
Error win32_error(const char* operation, DWORD error) {
return Error{
ErrorCode::service_error,
std::string(operation) + " failed with Win32 error " + std::to_string(error)};
}
Result<void> validate_supported_gpu(const P104UnlockRequest& request) {
if (!pseries::is_supported_unlock_gpu(
request.gpu_vendor_id, request.gpu_device_id)) {
return Result<void>{{Error{
ErrorCode::unsupported,
"P2UNLOCK supports P104-100 (10DE:1B87) and "
"P104-101 (10DE:1BC7) only"}}};
}
return Result<void>{};
}
class PnpEnableGuard final {
public:
explicit PnpEnableGuard(std::wstring instance_id)
: instance_id_(std::move(instance_id)) {
}
PnpEnableGuard(const PnpEnableGuard&) = delete;
PnpEnableGuard& operator=(const PnpEnableGuard&) = delete;
~PnpEnableGuard() {
if (active_) {
static_cast<void>(set_pci_device_enabled(instance_id_, true));
}
}
void arm() noexcept {
active_ = true;
}
void disarm() noexcept {
active_ = false;
}
[[nodiscard]] bool active() const noexcept {
return active_;
}
private:
std::wstring instance_id_;
bool active_{false};
};
Result<std::uint64_t> ensure_parent_directory(const std::wstring& path) {
const auto separator = path.find_last_of(L"\\/");
if (separator == std::wstring::npos || separator == 0) {
return Result<std::uint64_t>{std::uint64_t{0}};
}
const auto parent = path.substr(0, separator);
if (parent.empty()) {
return Result<std::uint64_t>{std::uint64_t{0}};
}
if (CreateDirectoryW(parent.c_str(), nullptr)) {
return Result<std::uint64_t>{std::uint64_t{0}};
}
const auto error = GetLastError();
if (error == ERROR_ALREADY_EXISTS) {
return Result<std::uint64_t>{std::uint64_t{0}};
}
return Result<std::uint64_t>{{win32_error("Create transaction journal directory", error)}};
}
Result<std::uint32_t> read_config(
const SeavoBackend& backend,
std::span<const McfgAllocation> allocations,
const PciBdf& bdf,
std::uint16_t offset,
std::uint8_t width) {
const auto address = pci_ecam_address(allocations, bdf, offset, width);
if (!address.has_value()) {
return Result<std::uint32_t>{{address.error()}};
}
return backend.read_physical(address.get(), width);
}
Result<void> write_config(
const SeavoBackend& backend,
std::span<const McfgAllocation> allocations,
const PciBdf& bdf,
std::uint16_t offset,
std::uint8_t width,
std::uint32_t value) {
const auto address = pci_ecam_address(allocations, bdf, offset, width);
if (!address.has_value()) {
return Result<void>{{address.error()}};
}
return backend.write_physical(address.get(), width, value);
}
Result<std::uint32_t> read_bar0(
const SeavoBackend& backend,
std::uint64_t bar0_base,
std::uint32_t offset,
std::uint8_t width) {
return backend.read_physical(bar0_base + offset, width);
}
Result<void> write_bar0(
const SeavoBackend& backend,
std::uint64_t bar0_base,
std::uint32_t offset,
std::uint8_t width,
std::uint32_t value) {
return backend.write_physical(bar0_base + offset, width, value);
}
Result<std::uint32_t> read_register(
const SeavoBackend& backend,
std::span<const McfgAllocation> allocations,
const PciBdf& gpu_bdf,
const PciBdf& upstream_bdf,
std::uint64_t bar0_base,
const JournalRegisterSnapshot& snapshot) {
switch (snapshot.kind) {
case JournalRegisterKind::gpu_config:
return read_config(backend, allocations, gpu_bdf, static_cast<std::uint16_t>(snapshot.offset), snapshot.width);
case JournalRegisterKind::upstream_config:
return read_config(backend, allocations, upstream_bdf, static_cast<std::uint16_t>(snapshot.offset), snapshot.width);
case JournalRegisterKind::gpu_bar0:
return read_bar0(backend, bar0_base, snapshot.offset, snapshot.width);
}
return Result<std::uint32_t>{{Error{
ErrorCode::invalid_argument,
"Journal contains an unsupported register target"}}};
}
Result<void> write_register(
const SeavoBackend& backend,
std::span<const McfgAllocation> allocations,
const PciBdf& gpu_bdf,
const PciBdf& upstream_bdf,
std::uint64_t bar0_base,
const JournalRegisterSnapshot& snapshot) {
switch (snapshot.kind) {
case JournalRegisterKind::gpu_config:
return write_config(
backend,
allocations,
gpu_bdf,
static_cast<std::uint16_t>(snapshot.offset),
snapshot.width,
snapshot.intended);
case JournalRegisterKind::upstream_config:
return write_config(
backend,
allocations,
upstream_bdf,
static_cast<std::uint16_t>(snapshot.offset),
snapshot.width,
snapshot.intended);
case JournalRegisterKind::gpu_bar0:
return write_bar0(
backend,
bar0_base,
snapshot.offset,
snapshot.width,
snapshot.intended);
}
return Result<void>{{Error{
ErrorCode::invalid_argument,
"Journal contains an unsupported register target"}}};
}
Result<void> apply_and_verify_register(
const SeavoBackend& backend,
std::span<const McfgAllocation> allocations,
const PciBdf& gpu_bdf,
const PciBdf& upstream_bdf,
std::uint64_t bar0_base,
const JournalRegisterSnapshot& snapshot,
bool allow_self_clearing_commit = false) {
const auto written = write_register(
backend, allocations, gpu_bdf, upstream_bdf, bar0_base, snapshot);
if (!written.has_value()) {
return written;
}
const auto observed = read_register(
backend, allocations, gpu_bdf, upstream_bdf, bar0_base, snapshot);
if (!observed.has_value()) {
return Result<void>{{observed.error()}};
}
const auto self_cleared_commit =
allow_self_clearing_commit &&
snapshot.kind == JournalRegisterKind::gpu_bar0 &&
snapshot.offset == 0x8C040 &&
(snapshot.intended & 0x1u) != 0 &&
observed.get() == (snapshot.intended & ~std::uint64_t{1});
const auto transient_bar0_unavailable =
snapshot.kind == JournalRegisterKind::gpu_bar0 &&
snapshot.offset == 0x8C040 &&
observed.get() == 0xFFFFFFFFu &&
pseries::is_p104_link_config_value_legal(snapshot.intended);
if (observed.get() != snapshot.intended &&
!self_cleared_commit &&
!transient_bar0_unavailable) {
return Result<void>{{Error{
ErrorCode::service_error,
"Register write verification failed at offset " +
std::to_string(snapshot.offset) + ": before " +
std::to_string(snapshot.before) + ", expected " +
std::to_string(snapshot.intended) + ", observed " +
std::to_string(observed.get())}}};
}
return Result<void>{};
}
template <typename Reader>
Result<void> wait_for_register_settled(
const Reader& read,
const JournalRegisterSnapshot& snapshot,
DWORD interval_ms = 20,
DWORD total_ms = 1000) {
const auto deadline = GetTickCount64() + total_ms;
bool readable = false;
while (GetTickCount64() < deadline) {
const auto current = read(snapshot);
if (current.has_value()) {
readable = true;
const auto self_clearing_commit =
snapshot.kind == JournalRegisterKind::gpu_bar0 &&
snapshot.offset == 0x8C040 &&
(snapshot.intended & 0x1u) != 0 &&
current.get() == (snapshot.intended & ~std::uint32_t{1});
if (current.get() == snapshot.intended || self_clearing_commit) {
return Result<void>{};
}
}
Sleep(interval_ms);
}
return Result<void>{{Error{
ErrorCode::service_error,
(readable ? "Register write did not settle at offset "
: "Register did not become readable after write at offset ") +
std::to_string(snapshot.offset)}}};
}
Result<std::uint64_t> resolve_bar0(
const SeavoBackend& backend,
std::span<const McfgAllocation> allocations,
const PciBdf& gpu_bdf) {
const auto low = read_config(backend, allocations, gpu_bdf, 0x10, 4);
if (!low.has_value()) {
return Result<std::uint64_t>{{low.error()}};
}
if ((low.get() & 0x1u) != 0) {
return Result<std::uint64_t>{{Error{
ErrorCode::unsupported,
"P104 BAR0 is not a memory BAR"}}};
}
std::uint64_t base = low.get() & 0xFFFFFFF0u;
if (((low.get() >> 1) & 0x3u) == 0x2u) {
const auto high = read_config(backend, allocations, gpu_bdf, 0x14, 4);
if (!high.has_value()) {
return Result<std::uint64_t>{{high.error()}};
}
base |= static_cast<std::uint64_t>(high.get()) << 32;
}
if (base == 0) {
return Result<std::uint64_t>{{Error{
ErrorCode::invalid_recovery_state,
"P104 BAR0 is not assigned"}}};
}
return Result<std::uint64_t>{base};
}
Result<std::uint8_t> verify_target_link(
const SeavoBackend& backend,
std::span<const McfgAllocation> allocations,
const PciBdf& gpu_bdf,
const PciBdf& upstream_bdf,
std::uint8_t requested_lanes,
std::uint8_t accepted_lanes,
LinkGeneration target_generation) {
for (unsigned attempt = 0; attempt < 40; ++attempt) {
const auto gpu = inspect_pcie_link(backend, allocations, gpu_bdf);
const auto upstream = inspect_pcie_link(backend, allocations, upstream_bdf);
if (gpu.has_value() && upstream.has_value() &&
gpu.get().link.current_generation == target_generation &&
upstream.get().link.current_generation == target_generation &&
gpu.get().link.negotiated_width.lanes ==
upstream.get().link.negotiated_width.lanes &&
(gpu.get().link.negotiated_width.lanes == requested_lanes ||
gpu.get().link.negotiated_width.lanes == accepted_lanes) &&
gpu.get().link.training_state == LinkTrainingState::stable &&
upstream.get().link.training_state == LinkTrainingState::stable) {
return Result<std::uint8_t>{gpu.get().link.negotiated_width.lanes};
}
Sleep(250);
}
std::string message = std::string("Link did not settle at ") +
(target_generation == LinkGeneration::gen1 ? "Gen1" : "Gen2") +
" x" + std::to_string(requested_lanes);
if (accepted_lanes != requested_lanes) {
message += " or the accepted fallback x" +
std::to_string(accepted_lanes);
}
message += " after the final reload";
return Result<std::uint8_t>{{Error{ErrorCode::unsupported, std::move(message)}}};
}
Result<void> load_ready_backend(
const P104UnlockRequest& request,
std::unique_ptr<SeavoBackend>& backend) {
std::string last_error = "Seavo driver was not loaded";
for (unsigned attempt = 0; attempt < 30; ++attempt) {
auto loaded = SeavoBackend::load(request.driver_path);
if (!loaded.has_value()) {
last_error = loaded.error().message;
Sleep(500);
continue;
}
const auto identity = read_config(
*loaded.get(), request.allocations, request.gpu_bdf, 0x00, 4);
const auto expected_identity =
(static_cast<std::uint32_t>(request.gpu_device_id) << 16) |
static_cast<std::uint32_t>(request.gpu_vendor_id);
if (identity.has_value() && identity.get() == expected_identity) {
backend = std::move(loaded.get());
return Result<void>{};
}
if (!identity.has_value()) {
last_error = identity.error().message;
} else {
last_error = "P104 identity was not readable after PnP enable";
}
static_cast<void>(loaded.get()->close());
Sleep(500);
}
return Result<void>{{Error{
ErrorCode::service_error,
"Timed out waiting for P104 after PnP change: " + last_error}}};
}
Result<P104PlanPreview> read_plan(
const SeavoBackend& backend,
const P104UnlockRequest& request) {
const auto supported = validate_supported_gpu(request);
if (!supported.has_value()) {
return Result<P104PlanPreview>{{supported.error()}};
}
const auto gpu_link = inspect_pcie_link(
backend, request.allocations, request.gpu_bdf);
const auto upstream_link = inspect_pcie_link(
backend, request.allocations, request.upstream_bdf);
if (!gpu_link.has_value() || !upstream_link.has_value()) {
return Result<P104PlanPreview>{{Error{
ErrorCode::hardware_not_found,
"Could not read the P104 and upstream PCIe links"}}};
}
if (gpu_link.get().vendor_id != request.gpu_vendor_id ||
gpu_link.get().device_id != request.gpu_device_id ||
upstream_link.get().vendor_id != request.upstream_vendor_id ||
upstream_link.get().device_id != request.upstream_device_id) {
return Result<P104PlanPreview>{{Error{
ErrorCode::hardware_not_found,
"PCIe link identities do not match the discovered GPU/upstream topology"}}};
}
const auto bar0_base = resolve_bar0(
backend, request.allocations, request.gpu_bdf);
if (!bar0_base.has_value()) {
return Result<P104PlanPreview>{{bar0_base.error()}};
}
if (!gpu_link.get().link.maximum_generation.has_value() ||
!upstream_link.get().link.maximum_generation.has_value() ||
*gpu_link.get().link.maximum_generation < LinkGeneration::gen1 ||
*upstream_link.get().link.maximum_generation < LinkGeneration::gen1) {
return Result<P104PlanPreview>{{Error{
ErrorCode::unsupported,
"Both P104 and upstream must advertise Gen1 or better"}}};
}
if (!gpu_link.get().link.maximum_width.has_value() ||
!upstream_link.get().link.maximum_width.has_value()) {
return Result<P104PlanPreview>{{Error{
ErrorCode::unsupported,
"GPU or upstream maximum link width is unavailable"}}};
}
const auto selection = resolve_p104_target_selection(
request.target_lanes,
gpu_link.get().link.maximum_width->lanes,
upstream_link.get().link.maximum_width->lanes);
if (!selection.has_value()) {
return Result<P104PlanPreview>{{selection.error()}};
}
const auto& target = selection.get().request;
const auto accepted_lanes = selection.get().accepted_lanes;
const auto target_generation = request.target_generation;
if (target_generation != LinkGeneration::gen1 &&
target_generation != LinkGeneration::gen2) {
return Result<P104PlanPreview>{{Error{
ErrorCode::unsupported,
"P104 unlock targets Gen1 or Gen2 only"}}};
}
const bool gpu_is_requested =
gpu_link.get().link.current_generation == target_generation &&
gpu_link.get().link.negotiated_width.lanes == target.lanes &&
gpu_link.get().link.training_state == LinkTrainingState::stable;
const bool upstream_is_requested =
upstream_link.get().link.current_generation == target_generation &&
upstream_link.get().link.negotiated_width.lanes == target.lanes &&
upstream_link.get().link.training_state == LinkTrainingState::stable;
const bool gpu_is_accepted =
gpu_link.get().link.current_generation == target_generation &&
gpu_link.get().link.negotiated_width.lanes == accepted_lanes &&
gpu_link.get().link.training_state == LinkTrainingState::stable;
const bool upstream_is_accepted =
upstream_link.get().link.current_generation == target_generation &&
upstream_link.get().link.negotiated_width.lanes == accepted_lanes &&
upstream_link.get().link.training_state == LinkTrainingState::stable;
if ((gpu_is_requested && upstream_is_requested) ||
(gpu_is_accepted && upstream_is_accepted)) {
P104PlanPreview preview{};
preview.gpu_link = gpu_link.get();
preview.upstream_link = upstream_link.get();
preview.bar0_base = bar0_base.get();
preview.target = target;
preview.accepted_lanes = accepted_lanes;
preview.target_generation = target_generation;
preview.width_change_needed = false;
preview.already_target = true;
preview.target_reached = gpu_is_requested && upstream_is_requested;
return Result<P104PlanPreview>{std::move(preview)};
}
const auto baseline_lanes = gpu_link.get().link.negotiated_width.lanes;
const auto baseline_generation = gpu_link.get().link.current_generation;
if ((baseline_generation != LinkGeneration::gen1 &&
baseline_generation != LinkGeneration::gen2) ||
upstream_link.get().link.current_generation != baseline_generation ||
baseline_lanes == 0 ||
upstream_link.get().link.negotiated_width.lanes != baseline_lanes ||
gpu_link.get().link.training_state != LinkTrainingState::stable ||
upstream_link.get().link.training_state != LinkTrainingState::stable) {
return Result<P104PlanPreview>{{Error{
ErrorCode::invalid_recovery_state,
"P104 is neither in a stable Gen1/Gen2 baseline at or below the "
"requested width nor at the requested target"}}};
}
P104InitialRegisters initial{};
const auto upstream_link_control_offset =
upstream_link.get().link.link_control_offset;
const auto upstream_link_control_2_offset =
upstream_link.get().link.link_control_2_offset;
const auto valid_upstream_offset = [](std::uint16_t offset) {
return offset != 0 && offset <= 0xFF && (offset & 0x03u) == 0;
};
if (!valid_upstream_offset(upstream_link_control_offset) ||
!valid_upstream_offset(upstream_link_control_2_offset)) {
return Result<P104PlanPreview>{{Error{
ErrorCode::unsupported,
"Upstream PCIe capability offsets are unavailable"}}};
}
const auto upstream_3e = read_config(
backend, request.allocations, request.upstream_bdf, 0x3E, 2);
const auto upstream_link_control = read_config(
backend,
request.allocations,
request.upstream_bdf,
upstream_link_control_offset,
1);
const auto upstream_link_control_2 = read_config(
backend,
request.allocations,
request.upstream_bdf,
upstream_link_control_2_offset,
1);
const auto gpu_04 = read_config(
backend, request.allocations, request.gpu_bdf, 0x04, 2);
const auto gpu_10 = read_config(
backend, request.allocations, request.gpu_bdf, 0x10, 4);
const auto gpu_a8 = read_config(
backend, request.allocations, request.gpu_bdf, 0xA8, 2);
const auto bar0_88610 = read_bar0(backend, bar0_base.get(), 0x88610, 4);
const auto bar0_8841c = read_bar0(backend, bar0_base.get(), 0x8841C, 4);
const auto bar0_8872c = read_bar0(backend, bar0_base.get(), 0x8872C, 4);
const auto bar0_8c040 = read_bar0(backend, bar0_base.get(), 0x8C040, 4);
if (!upstream_3e.has_value() || !upstream_link_control.has_value() ||
!upstream_link_control_2.has_value() || !gpu_04.has_value() ||
!gpu_10.has_value() || !gpu_a8.has_value() ||
!bar0_88610.has_value() || !bar0_8841c.has_value() ||
!bar0_8872c.has_value() || !bar0_8c040.has_value()) {
return Result<P104PlanPreview>{{Error{
ErrorCode::service_error,
"Could not read all P104 preflight registers"}}};
}
initial.upstream_3e = upstream_3e.get();
initial.upstream_link_control = upstream_link_control.get();
initial.upstream_link_control_2 = upstream_link_control_2.get();
initial.upstream_link_control_offset = upstream_link_control_offset;
initial.upstream_link_control_2_offset = upstream_link_control_2_offset;
initial.gpu_04 = gpu_04.get();
initial.gpu_10 = gpu_10.get();
initial.gpu_a8 = gpu_a8.get();
initial.bar0_88610 = bar0_88610.get();
initial.bar0_8841c = bar0_8841c.get();
initial.bar0_8872c = bar0_8872c.get();
initial.bar0_8c040 = bar0_8c040.get();
const bool width_change_needed = baseline_lanes != target.lanes;
auto plan = build_p104_policy_and_width_plan(
initial, target.lanes, target_generation, width_change_needed);
if (!plan.has_value()) {
return Result<P104PlanPreview>{{plan.error()}};
}
P104PlanPreview preview{};
preview.gpu_link = gpu_link.get();
preview.upstream_link = upstream_link.get();
preview.bar0_base = bar0_base.get();
preview.initial = initial;
preview.target = target;
preview.accepted_lanes = accepted_lanes;
preview.target_generation = target_generation;
preview.width_change_needed = width_change_needed;
preview.policy_and_width = std::move(plan.get());
return Result<P104PlanPreview>{std::move(preview)};
}
Result<void> validate_declaration_request(const P104DeclarationRequest& request) {
if (request.driver_path.empty() || request.allocations.empty()) {
return Result<void>{{Error{
ErrorCode::invalid_argument,
"P104 declaration requires a driver and MCFG map"}}};
}
if (!pseries::is_supported_unlock_gpu(
request.gpu_vendor_id, request.gpu_device_id)) {
return Result<void>{{Error{
ErrorCode::unsupported,
"P2UNLOCK supports P104-100 (10DE:1B87) and "
"P104-101 (10DE:1BC7) only"}}};
}
return Result<void>{};
}
Result<std::unique_ptr<SeavoBackend>> load_declaration_backend(
const P104DeclarationRequest& request) {
auto loaded = SeavoBackend::load(request.driver_path);
if (!loaded.has_value()) {
return Result<std::unique_ptr<SeavoBackend>>{{loaded.error()}};
}
const auto identity = read_config(
*loaded.get(), request.allocations, request.gpu_bdf, 0x00, 4);
const auto expected_identity =
(static_cast<std::uint32_t>(request.gpu_device_id) << 16) |
static_cast<std::uint32_t>(request.gpu_vendor_id);
if (!identity.has_value() || identity.get() != expected_identity) {
const std::string message = identity.has_value()
? "P104 identity changed while loading the declaration backend"
: identity.error().message;
static_cast<void>(loaded.get()->close());
return Result<std::unique_ptr<SeavoBackend>>{{Error{
ErrorCode::hardware_not_found, message}}};
}
return Result<std::unique_ptr<SeavoBackend>>{std::move(loaded.get())};
}
Result<P104DeclarationStatus> read_declaration_status(
const SeavoBackend& backend,
const P104DeclarationRequest& request) {
const auto supported = validate_declaration_request(request);
if (!supported.has_value()) {
return Result<P104DeclarationStatus>{{supported.error()}};
}
const auto gpu_link = inspect_pcie_link(
backend, request.allocations, request.gpu_bdf);
const auto upstream_link = inspect_pcie_link(
backend, request.allocations, request.upstream_bdf);
if (!gpu_link.has_value() || !upstream_link.has_value()) {
return Result<P104DeclarationStatus>{{Error{
ErrorCode::hardware_not_found,
"Could not read the P104 and upstream PCIe links"}}};
}
if (gpu_link.get().vendor_id != request.gpu_vendor_id ||
gpu_link.get().device_id != request.gpu_device_id ||
upstream_link.get().vendor_id != request.upstream_vendor_id ||
upstream_link.get().device_id != request.upstream_device_id) {
return Result<P104DeclarationStatus>{{Error{
ErrorCode::hardware_not_found,
"PCIe identities do not match the discovered GPU/upstream topology"}}};
}
const auto bar0_base = resolve_bar0(
backend, request.allocations, request.gpu_bdf);
if (!bar0_base.has_value()) {
return Result<P104DeclarationStatus>{{bar0_base.error()}};
}
const auto link_config = read_bar0(
backend, bar0_base.get(), kP104LinkConfig0Offset, 4);
if (!link_config.has_value()) {
return Result<P104DeclarationStatus>{{link_config.error()}};
}
P104DeclarationStatus status{};
status.gpu_link = gpu_link.get();
status.upstream_link = upstream_link.get();
status.bar0_base = bar0_base.get();
status.link_config = link_config.get();
status.declared_width = gpu_link.get().link.maximum_width.has_value()
? gpu_link.get().link.maximum_width->lanes
: 0;
status.negotiated_width = gpu_link.get().link.negotiated_width.lanes;
status.stable_link =
(gpu_link.get().link.current_generation == LinkGeneration::gen1 ||
gpu_link.get().link.current_generation == LinkGeneration::gen2) &&
gpu_link.get().link.current_generation ==
upstream_link.get().link.current_generation &&
gpu_link.get().link.training_state == LinkTrainingState::stable &&
upstream_link.get().link.training_state == LinkTrainingState::stable &&
status.negotiated_width != 0 &&
status.negotiated_width ==
upstream_link.get().link.negotiated_width.lanes;
return Result<P104DeclarationStatus>{std::move(status)};
}
Result<void> require_declaration_precondition(
const P104DeclarationStatus& status) {
if (!status.stable_link) {
return Result<void>{{Error{
ErrorCode::invalid_recovery_state,
"Both P104 and upstream must be at a stable matching Gen1 or Gen2 "
"link before the decorative declaration is written"}}};
}
if (status.declared_width == 0) {
return Result<void>{{Error{
ErrorCode::unsupported,
"GPU LNKCAP maximum width is unavailable"}}};
}
return Result<void>{};
}
Result<P104DeclarationStatus> apply_declaration(
const SeavoBackend& backend,
const P104DeclarationRequest& request,
std::uint8_t lanes) {
const auto current = read_declaration_status(backend, request);
if (!current.has_value()) {
return Result<P104DeclarationStatus>{{current.error()}};
}
const auto precondition = require_declaration_precondition(current.get());
if (!precondition.has_value()) {
return Result<P104DeclarationStatus>{{precondition.error()}};
}
const auto intended = build_p104_declaration_value(
current.get().link_config, lanes);
if (!intended.has_value()) {
return Result<P104DeclarationStatus>{{intended.error()}};
}
if ((current.get().link_config & kP104LinkConfigWidthMask) ==
(intended.get() & kP104LinkConfigWidthMask) &&
current.get().declared_width == lanes) {
return current;
}
if (request.progress) {
const std::string message =
"declaration x" + std::to_string(lanes) +
": writing GPU BAR0 0x8C040 declaration only";
request.progress(message.c_str());
}
const auto written = write_bar0(
backend,
current.get().bar0_base,
kP104LinkConfig0Offset,
4,
intended.get());
if (!written.has_value()) {
return Result<P104DeclarationStatus>{{written.error()}};
}
Sleep(50);
const auto observed = read_bar0(
backend,
current.get().bar0_base,
kP104LinkConfig0Offset,
4);
if (!observed.has_value() || observed.get() != intended.get()) {
static_cast<void>(write_bar0(
backend,
current.get().bar0_base,
kP104LinkConfig0Offset,
4,
current.get().link_config));
return Result<P104DeclarationStatus>{{Error{
ErrorCode::service_error,
"Declaration did not land in GPU BAR0"}}};
}
for (unsigned attempt = 0; attempt < 10; ++attempt) {
const auto gpu_link = inspect_pcie_link(
backend, request.allocations, request.gpu_bdf);
if (gpu_link.has_value() &&
gpu_link.get().link.maximum_width.has_value() &&
gpu_link.get().link.maximum_width->lanes == lanes) {
auto result = current.get();
result.gpu_link = gpu_link.get();
result.declared_width = lanes;
result.link_config = intended.get();
if (request.progress) {
const std::string message =
"declaration x" + std::to_string(lanes) +
": LNKCAP now matches; physical width unchanged";
request.progress(message.c_str());
}
return Result<P104DeclarationStatus>{std::move(result)};
}
Sleep(100);
}
static_cast<void>(write_bar0(
backend,
current.get().bar0_base,
kP104LinkConfig0Offset,
4,
current.get().link_config));
return Result<P104DeclarationStatus>{{Error{
ErrorCode::unsupported,
"GPU did not report the requested LNKCAP declaration width"}}};
}
std::atomic_bool g_declaration_stop{false};
BOOL WINAPI declaration_ctrl_handler(DWORD control_type) {
if (control_type == CTRL_C_EVENT ||
control_type == CTRL_BREAK_EVENT ||
control_type == CTRL_CLOSE_EVENT) {
g_declaration_stop.store(true);
return TRUE;
}
return FALSE;
}
}
Result<P104DeclarationStatus> inspect_p104_declaration(
const P104DeclarationRequest& request) {
const auto supported = validate_declaration_request(request);
if (!supported.has_value()) {
return Result<P104DeclarationStatus>{{supported.error()}};
}
auto backend = load_declaration_backend(request);
if (!backend.has_value()) {
return Result<P104DeclarationStatus>{{backend.error()}};
}
const auto status = read_declaration_status(*backend.get(), request);
static_cast<void>(backend.get()->close());
return status;
}
Result<P104DeclarationStatus> execute_p104_decorative_x16(
const P104DeclarationRequest& request) {
const auto supported = validate_declaration_request(request);
if (!supported.has_value()) {
return Result<P104DeclarationStatus>{{supported.error()}};
}
auto backend = load_declaration_backend(request);
if (!backend.has_value()) {
return Result<P104DeclarationStatus>{{backend.error()}};
}
const auto result = apply_declaration(*backend.get(), request, 16);
static_cast<void>(backend.get()->close());
return result;
}
Result<P104DeclarationStatus> execute_p104_declaration_negotiated(
const P104DeclarationRequest& request) {
const auto supported = validate_declaration_request(request);
if (!supported.has_value()) {
return Result<P104DeclarationStatus>{{supported.error()}};
}
auto backend = load_declaration_backend(request);
if (!backend.has_value()) {
return Result<P104DeclarationStatus>{{backend.error()}};
}
const auto status = read_declaration_status(*backend.get(), request);
if (!status.has_value()) {
static_cast<void>(backend.get()->close());
return Result<P104DeclarationStatus>{{status.error()}};
}
const auto result = apply_declaration(
*backend.get(), request, status.get().negotiated_width);
static_cast<void>(backend.get()->close());
return result;
}
Result<void> hold_p104_decorative_x16(const P104DeclarationRequest& request) {
const auto supported = validate_declaration_request(request);
if (!supported.has_value()) {
return Result<void>{{supported.error()}};
}
g_declaration_stop.store(false);
if (!SetConsoleCtrlHandler(declaration_ctrl_handler, TRUE)) {
return Result<void>{{win32_error(
"Install declaration stop handler", GetLastError())}};
}
auto backend = load_declaration_backend(request);
if (!backend.has_value()) {
static_cast<void>(SetConsoleCtrlHandler(declaration_ctrl_handler, FALSE));
return Result<void>{{backend.error()}};
}
while (!g_declaration_stop.load()) {
const auto status = read_declaration_status(*backend.get(), request);
if (!status.has_value()) {
static_cast<void>(backend.get()->close());
static_cast<void>(SetConsoleCtrlHandler(declaration_ctrl_handler, FALSE));
return Result<void>{{status.error()}};
}
if (!status.get().stable_link) {
if (request.progress) {
request.progress("declaration hold: waiting for a stable matching link");
}
Sleep(200);
continue;
}
if (status.get().declared_width != 16) {
const auto applied = apply_declaration(*backend.get(), request, 16);
if (!applied.has_value()) {
static_cast<void>(backend.get()->close());
static_cast<void>(SetConsoleCtrlHandler(declaration_ctrl_handler, FALSE));
return Result<void>{{applied.error()}};
}
}
Sleep(200);
}
static_cast<void>(backend.get()->close());
static_cast<void>(SetConsoleCtrlHandler(declaration_ctrl_handler, FALSE));
return Result<void>{};
}
Result<P104PlanPreview> inspect_p104_plan(const P104UnlockRequest& request) {
if (request.driver_path.empty() ||
request.gpu_instance_id.empty() ||
request.allocations.empty()) {
return Result<P104PlanPreview>{{Error{
ErrorCode::invalid_argument,
"P104 plan requires a driver, instance ID, and MCFG map"}}};
}
std::unique_ptr<SeavoBackend> backend;
const auto loaded = load_ready_backend(request, backend);
if (!loaded.has_value()) {
return Result<P104PlanPreview>{{loaded.error()}};
}
const auto preview = read_plan(*backend, request);
const auto cleanup = backend->close();
if (!preview.has_value()) {
return Result<P104PlanPreview>{{preview.error()}};
}
if (!cleanup.has_value()) {
return Result<P104PlanPreview>{{cleanup.error()}};
}
return preview;
}
Result<void> reconcile_p104_journal(const P104UnlockRequest& request) {
if (request.journal_path.empty()) {
return Result<void>{{Error{
ErrorCode::invalid_argument, "P104 reconciliation requires a journal path"}}};
}
const auto attributes = GetFileAttributesW(request.journal_path.c_str());
const auto probe_error = GetLastError();
if (attributes == INVALID_FILE_ATTRIBUTES) {
if (probe_error != ERROR_FILE_NOT_FOUND &&
probe_error != ERROR_PATH_NOT_FOUND) {
return Result<void>{{win32_error("Probe transaction journal", probe_error)}};
}
return Result<void>{};
}
const auto prior = load_transaction_journal(request.journal_path);
if (!prior.has_value()) {
return Result<void>{{Error{
ErrorCode::invalid_recovery_state,
"An unreadable transaction journal exists; recover or remove it "
"before a new unlock"}}};
}
if (prior.get().phase == P104TransactionPhase::committed) {
return clear_transaction_journal(request.journal_path);
}
if (prior.get().phase != P104TransactionPhase::recovery_required) {
return Result<void>{{Error{
ErrorCode::invalid_recovery_state,
"An unfinished transaction journal exists; recover or remove it "
"before a new unlock"}}};
}
const auto supported = validate_supported_gpu(request);
if (!supported.has_value()) {
return Result<void>{{supported.error()}};
}
auto loaded = SeavoBackend::load(request.driver_path);
if (!loaded.has_value()) {
return Result<void>{{loaded.error()}};
}
const auto gpu = inspect_pcie_link(
*loaded.get(), request.allocations, request.gpu_bdf);
const auto upstream = inspect_pcie_link(
*loaded.get(), request.allocations, request.upstream_bdf);
if (!gpu.has_value() || !upstream.has_value()) {
static_cast<void>(loaded.get()->close());
return Result<void>{{Error{
ErrorCode::hardware_not_found,
"Could not read the P104 and upstream links during journal reconciliation"}}};
}
const auto generation = gpu.get().link.current_generation;
const auto lanes = gpu.get().link.negotiated_width.lanes;
if ((generation != LinkGeneration::gen1 &&
generation != LinkGeneration::gen2) ||
generation != prior.get().target_generation ||
lanes != prior.get().accepted_width_ceiling.lanes ||
upstream.get().link.current_generation != generation ||
upstream.get().link.negotiated_width.lanes != lanes ||
gpu.get().link.training_state != LinkTrainingState::stable ||
upstream.get().link.training_state != LinkTrainingState::stable) {
static_cast<void>(loaded.get()->close());
return Result<void>{{Error{
ErrorCode::invalid_recovery_state,
"An unfinished transaction journal exists and the link is not at "
"the recorded target; recover or remove it before a new unlock"}}};
}
std::uint32_t bar0_before = 0;
std::uint32_t command_before = 0;
bool have_bar0 = false;
bool have_command = false;
for (const auto& snapshot : prior.get().snapshots) {
if (snapshot.kind != JournalRegisterKind::gpu_config) {
continue;
}
if (snapshot.offset == 0x10 && !have_bar0) {
bar0_before = snapshot.before;
have_bar0 = true;
}
if (snapshot.offset == 0x04 && !have_command) {
command_before = snapshot.before;
have_command = true;
}
}
auto bar0 = resolve_bar0(*loaded.get(), request.allocations, request.gpu_bdf);
if (!bar0.has_value() && have_bar0) {
const auto restored_bar0 = write_config(
*loaded.get(), request.allocations, request.gpu_bdf,
0x10, 4, bar0_before);
if (!restored_bar0.has_value()) {
static_cast<void>(loaded.get()->close());
return Result<void>{{restored_bar0.error()}};
}
if (have_command) {
const auto restored_command = write_config(
*loaded.get(), request.allocations, request.gpu_bdf,
0x04, 2, command_before);
if (!restored_command.has_value()) {
static_cast<void>(loaded.get()->close());
return Result<void>{{restored_command.error()}};
}
}
Sleep(250);
bar0 = resolve_bar0(*loaded.get(), request.allocations, request.gpu_bdf);
}
static_cast<void>(loaded.get()->close());
if (!bar0.has_value()) {
return Result<void>{{bar0.error()}};
}
return clear_transaction_journal(request.journal_path);
}
Result<P104TransactionJournal> execute_p104_unlock(
const P104UnlockRequest& request) {
if (request.driver_path.empty() ||
request.journal_path.empty() ||
request.gpu_instance_id.empty() ||
request.allocations.empty()) {
return Result<P104TransactionJournal>{{Error{
ErrorCode::invalid_argument,
"P104 unlock requires a driver, journal, instance ID, and MCFG map"}}};
}
const auto supported = validate_supported_gpu(request);
if (!supported.has_value()) {
return Result<P104TransactionJournal>{{supported.error()}};
}
const auto existing_journal =
GetFileAttributesW(request.journal_path.c_str());
const auto journal_probe_error = GetLastError();
if (existing_journal != INVALID_FILE_ATTRIBUTES) {
const auto prior = load_transaction_journal(request.journal_path);
if (!prior.has_value()) {
return Result<P104TransactionJournal>{{Error{
ErrorCode::invalid_recovery_state,
"An unreadable transaction journal exists; recover or remove it "
"before a new unlock"}}};
}
if (prior.get().phase != P104TransactionPhase::committed) {
return Result<P104TransactionJournal>{{Error{
ErrorCode::invalid_recovery_state,
"An unfinished transaction journal exists; recover or remove it "
"before a new unlock"}}};
}
const auto cleared = clear_transaction_journal(request.journal_path);
if (!cleared.has_value()) {
return Result<P104TransactionJournal>{{cleared.error()}};
}
} else if (journal_probe_error != ERROR_FILE_NOT_FOUND &&
journal_probe_error != ERROR_PATH_NOT_FOUND) {
return Result<P104TransactionJournal>{{win32_error(
"Probe transaction journal", journal_probe_error)}};
}
const auto directory = ensure_parent_directory(request.journal_path);
if (!directory.has_value()) {
return Result<P104TransactionJournal>{{directory.error()}};
}
std::unique_ptr<SeavoBackend> backend;
const auto initial_load = load_ready_backend(request, backend);
if (!initial_load.has_value()) {
return Result<P104TransactionJournal>{{initial_load.error()}};
}
const auto reload_backend = [&request, &backend]() {
return load_ready_backend(request, backend);
};
PnpEnableGuard pnp_guard(request.gpu_instance_id);
const auto preview = read_plan(*backend, request);
if (!preview.has_value()) {
static_cast<void>(backend->close());
return Result<P104TransactionJournal>{{preview.error()}};
}
if (preview.get().already_target) {
static_cast<void>(backend->close());
const auto observed_lanes = preview.get().gpu_link.link.negotiated_width.lanes;
const auto observed_generation = preview.get().gpu_link.link.current_generation;
std::string message = std::string("P104 is already at ") +
(observed_generation == LinkGeneration::gen1 ? "Gen1" : "Gen2") +
" x" + std::to_string(observed_lanes);
if (!preview.get().target_reached) {
message += "; requested x" +
std::to_string(preview.get().target.lanes) +
" was not reached";
}
message += "; no writes were performed";
return Result<P104TransactionJournal>{{Error{
ErrorCode::unsupported, std::move(message)}}};
}
P104TransactionJournal journal{};
journal.transaction_id =
static_cast<std::uint64_t>(GetTickCount64()) ^
(static_cast<std::uint64_t>(GetCurrentProcessId()) << 32);
if (journal.transaction_id == 0) {
journal.transaction_id = 1;
}
journal.gpu_bdf = request.gpu_bdf;
journal.upstream_bdf = request.upstream_bdf;
journal.gpu_vendor_id = request.gpu_vendor_id;
journal.gpu_device_id = request.gpu_device_id;
journal.upstream_vendor_id = request.upstream_vendor_id;
journal.upstream_device_id = request.upstream_device_id;
journal.initial_generation = preview.get().gpu_link.link.current_generation;
journal.initial_width.lanes = preview.get().gpu_link.link.negotiated_width.lanes;
journal.target_generation = preview.get().target_generation;
journal.accepted_width_ceiling.lanes = preview.get().accepted_lanes;
journal.phase = P104TransactionPhase::prepared;
journal.snapshots = preview.get().policy_and_width;
const auto persist = [&request](const P104TransactionJournal& state) {
return save_transaction_journal(request.journal_path, state);
};
const auto set_phase = [&journal, &persist](P104TransactionPhase next) {
const auto advanced = advance_transaction_phase(journal.phase, next);
if (!advanced.has_value()) {
return Result<void>{{advanced.error()}};
}
journal.phase = advanced.get();
return persist(journal);
};
const auto fail_transaction = [&journal, &persist](Error error) {
journal.phase = P104TransactionPhase::recovery_required;
const auto saved = persist(journal);
if (!saved.has_value()) {
error.message += "; failed to persist recovery-required journal: ";
error.message += saved.error().message;
}
return Result<void>{{std::move(error)}};
};
const auto abort_transaction = [&](const Error& error) {
static_cast<void>(fail_transaction(error));
if (backend) {
static_cast<void>(backend->close());
}
return Result<P104TransactionJournal>{{error}};
};
std::uint64_t current_bar0 = preview.get().bar0_base;
const auto read_current = [&backend, &request, &current_bar0](
const JournalRegisterSnapshot& snapshot) {
return read_register(
*backend,
request.allocations,
request.gpu_bdf,
request.upstream_bdf,
current_bar0,
snapshot);
};
const auto apply_write = [&backend, &request, &current_bar0](
const JournalRegisterSnapshot& snapshot) {
return apply_and_verify_register(
*backend,
request.allocations,
request.gpu_bdf,
request.upstream_bdf,
current_bar0,
snapshot,
true);
};
const auto apply_batch = [&](std::size_t begin, std::size_t end) -> Result<void> {
if (begin >= end || end > journal.snapshots.size()) {
return Result<void>{{Error{
ErrorCode::invalid_argument,
"Journal write batch is outside the prepared register range"}}};
}
for (std::size_t index = begin; index < end; ++index) {
journal.snapshots[index].state = JournalWriteState::pending;
}
const auto marked = persist(journal);
if (!marked.has_value()) {
return fail_transaction(marked.error());
}
for (std::size_t index = begin; index < end; ++index) {
auto& snapshot = journal.snapshots[index];
const auto current = read_current(snapshot);
if (!current.has_value()) {
return fail_transaction(current.error());
}
snapshot.before = current.get();
const auto written = apply_write(snapshot);
if (!written.has_value()) {
return fail_transaction(written.error());
}
snapshot.state = JournalWriteState::applied;
if (index + 1 < end) {
const auto settled = wait_for_register_settled(read_current, snapshot);
if (!settled.has_value()) {
return fail_transaction(settled.error());
}
}
}
const auto saved = persist(journal);
if (!saved.has_value()) {
return fail_transaction(saved.error());
}
return Result<void>{};
};
const auto apply_batch_raw = [&](std::size_t begin, std::size_t end) -> Result<void> {
if (begin >= end || end > journal.snapshots.size()) {
return Result<void>{{Error{
ErrorCode::invalid_argument,
"Journal write batch is outside the prepared register range"}}};
}
for (std::size_t index = begin; index < end; ++index) {
journal.snapshots[index].state = JournalWriteState::pending;
}
const auto marked = persist(journal);
if (!marked.has_value()) {
return fail_transaction(marked.error());
}
for (std::size_t index = begin; index < end; ++index) {
auto& snapshot = journal.snapshots[index];
const auto current = read_current(snapshot);
if (!current.has_value()) {
return fail_transaction(current.error());
}
snapshot.before = current.get();
const auto written = write_register(
*backend,
request.allocations,
request.gpu_bdf,
request.upstream_bdf,
current_bar0,
snapshot);
if (!written.has_value()) {
return fail_transaction(written.error());
}
snapshot.state = JournalWriteState::applied;
if (index + 1 < end) {
Sleep(100);
}
}
const auto saved = persist(journal);
if (!saved.has_value()) {
return fail_transaction(saved.error());
}
return Result<void>{};
};
const auto prepared = persist(journal);
if (!prepared.has_value()) {
static_cast<void>(backend->close());
return Result<P104TransactionJournal>{{prepared.error()}};
}
if (request.progress) {
request.progress("prepared");
}
const auto policy_phase = set_phase(P104TransactionPhase::policy_armed);
if (!policy_phase.has_value()) {
static_cast<void>(backend->close());
return Result<P104TransactionJournal>{{policy_phase.error()}};
}
const auto policy_write = apply_journaled_write(
journal, 0, persist, read_current, apply_write);
if (!policy_write.has_value()) {
static_cast<void>(backend->close());
return Result<P104TransactionJournal>{{policy_write.error()}};
}
if (request.progress) {
request.progress("policy-armed");
}
const bool has_width_phase = preview.get().width_change_needed;
const bool disable_for_width = has_width_phase && !request.skip_pnp_disable;
if (!has_width_phase) {
if (request.progress) {
request.progress("width-phase-skipped");
}
} else if (!request.skip_pnp_disable) {
const auto disabled =
set_pci_device_enabled(request.gpu_instance_id, false, false);
if (!disabled.has_value()) {
return abort_transaction(disabled.error());
}
pnp_guard.arm();
} else if (request.progress) {
request.progress("pnp-disable-skipped");
}
const auto disabled_phase = set_phase(P104TransactionPhase::gpu_disabled);
if (!disabled_phase.has_value()) {
return abort_transaction(disabled_phase.error());
}
const auto temporary_phase = set_phase(P104TransactionPhase::temporary_access);
if (!temporary_phase.has_value()) {
return abort_transaction(temporary_phase.error());
}
if (has_width_phase) {
const auto width_applied = apply_batch_raw(1, journal.snapshots.size());
if (!width_applied.has_value()) {
return abort_transaction(width_applied.error());
}
}
const auto width_phase = set_phase(P104TransactionPhase::width_configured);
if (!width_phase.has_value()) {
return abort_transaction(width_phase.error());
}
if (has_width_phase) {
Sleep(250);
}
const auto retrained_phase = set_phase(P104TransactionPhase::link_retrained);
if (!retrained_phase.has_value()) {
return abort_transaction(retrained_phase.error());
}
if (request.progress) {
request.progress(has_width_phase ? "width-configured" : "width-phase-skipped");
}
if (has_width_phase && request.skip_pnp_disable) {
const auto restore_start = journal.snapshots.size();
journal.snapshots.push_back(JournalRegisterSnapshot{
JournalRegisterKind::gpu_config,
0x10,
4,
JournalWriteState::planned,
0,
static_cast<std::uint32_t>(current_bar0)});
journal.snapshots.push_back(JournalRegisterSnapshot{
JournalRegisterKind::gpu_config,
0x04,
2,
JournalWriteState::planned,
0,
0x0006});
journal.snapshots.push_back(JournalRegisterSnapshot{
JournalRegisterKind::gpu_bar0,
0x8C040,
4,
JournalWriteState::planned,
0,
preview.get().target.speed_pre_commit});
const auto restore_saved = persist(journal);
if (!restore_saved.has_value()) {
return abort_transaction(restore_saved.error());
}
const auto restored = apply_batch_raw(
restore_start, journal.snapshots.size());
if (!restored.has_value()) {
return abort_transaction(restored.error());
}
if (request.progress) {
request.progress("policy-restored-skip-pnp");
}
}
if (pnp_guard.active()) {
static_cast<void>(backend->close());
backend.reset();
const auto enabled = set_pci_device_enabled(request.gpu_instance_id, true);
if (!enabled.has_value()) {
return abort_transaction(enabled.error());
}
pnp_guard.disarm();
}
const auto enabled_phase = set_phase(P104TransactionPhase::gpu_enabled);
if (!enabled_phase.has_value()) {
return abort_transaction(enabled_phase.error());
}
if (disable_for_width) {
const auto reloaded = reload_backend();
if (!reloaded.has_value()) {
static_cast<void>(fail_transaction(reloaded.error()));
return Result<P104TransactionJournal>{{reloaded.error()}};
}
const auto new_bar0 = resolve_bar0(
*backend, request.allocations, request.gpu_bdf);
if (!new_bar0.has_value()) {
return abort_transaction(new_bar0.error());
}
current_bar0 = new_bar0.get();
}
P104SpeedRegisters speed{};
const auto speed_upstream_link_control_2_offset =
preview.get().upstream_link.link.link_control_2_offset;
if (speed_upstream_link_control_2_offset == 0 ||
speed_upstream_link_control_2_offset > 0xFF ||
(speed_upstream_link_control_2_offset & 0x03u) != 0) {
return abort_transaction(Error{
ErrorCode::unsupported,
"Upstream Link Control 2 offset is unavailable"});
}
const auto speed_upstream_link_control_2 = read_config(
*backend,
request.allocations,
request.upstream_bdf,
speed_upstream_link_control_2_offset,
1);
const auto speed_gpu_a8 = read_config(
*backend, request.allocations, request.gpu_bdf, 0xA8, 2);
const auto speed_88610 = read_bar0(*backend, current_bar0, 0x88610, 4);
const auto speed_8841c = read_bar0(*backend, current_bar0, 0x8841C, 4);
const auto speed_8872c = read_bar0(*backend, current_bar0, 0x8872C, 4);
const auto speed_8c040 = read_bar0(*backend, current_bar0, 0x8C040, 4);
if (!speed_upstream_link_control_2.has_value() ||
!speed_gpu_a8.has_value() || !speed_88610.has_value() ||
!speed_8841c.has_value() || !speed_8872c.has_value() ||
!speed_8c040.has_value()) {
return abort_transaction(Error{
ErrorCode::service_error,
"Could not read all P104 speed-phase registers"});
}
speed.upstream_link_control_2 = speed_upstream_link_control_2.get();
speed.upstream_link_control_2_offset =
speed_upstream_link_control_2_offset;
speed.gpu_a8 = speed_gpu_a8.get();
speed.bar0_88610 = speed_88610.get();
speed.bar0_8841c = speed_8841c.get();
speed.bar0_8872c = speed_8872c.get();
speed.bar0_8c040 = speed_8c040.get();
const auto speed_plan = build_p104_speed_plan(
speed, preview.get().target.lanes, preview.get().target_generation);
if (!speed_plan.has_value()) {
return abort_transaction(speed_plan.error());
}
const auto speed_start = journal.snapshots.size();
journal.snapshots.insert(
journal.snapshots.end(),
speed_plan.get().begin(),
speed_plan.get().end());
const auto speed_saved = persist(journal);
if (!speed_saved.has_value()) {
return abort_transaction(speed_saved.error());
}
const auto speed_applied = apply_batch(speed_start, journal.snapshots.size());
if (!speed_applied.has_value()) {
return abort_transaction(speed_applied.error());
}
const auto speed_phase = set_phase(P104TransactionPhase::speed_configured);
if (!speed_phase.has_value()) {
return abort_transaction(speed_phase.error());
}
if (request.progress) {
request.progress("speed-configured");
}
static_cast<void>(backend->close());
backend.reset();
const auto reload_disabled =
set_pci_device_enabled(request.gpu_instance_id, false, false);
if (!reload_disabled.has_value()) {
return abort_transaction(reload_disabled.error());
}
pnp_guard.arm();
const auto reload_enabled =
set_pci_device_enabled(request.gpu_instance_id, true);
if (!reload_enabled.has_value()) {
return abort_transaction(reload_enabled.error());
}
pnp_guard.disarm();
const auto final_reloaded = reload_backend();
if (!final_reloaded.has_value()) {
return abort_transaction(final_reloaded.error());
}
const auto final_bar0 = resolve_bar0(
*backend, request.allocations, request.gpu_bdf);
if (!final_bar0.has_value()) {
return abort_transaction(final_bar0.error());
}
current_bar0 = final_bar0.get();
const auto final_gpu_a8 = read_config(
*backend, request.allocations, request.gpu_bdf, 0xA8, 2);
if (!final_gpu_a8.has_value()) {
return abort_transaction(final_gpu_a8.error());
}
const auto final_gpu_a8_intended =
preview.get().target_generation == LinkGeneration::gen1
? std::uint32_t{0x0001}
: std::uint32_t{0x0002};
journal.snapshots.push_back(JournalRegisterSnapshot{
JournalRegisterKind::gpu_config,
0xA8,
2,
JournalWriteState::planned,
final_gpu_a8.get(),
final_gpu_a8_intended});
const auto final_saved = persist(journal);
if (!final_saved.has_value()) {
return abort_transaction(final_saved.error());
}
const auto final_applied =
apply_batch(journal.snapshots.size() - 1, journal.snapshots.size());
if (!final_applied.has_value()) {
return abort_transaction(final_applied.error());
}
const auto verified = verify_target_link(
*backend,
request.allocations,
request.gpu_bdf,
request.upstream_bdf,
preview.get().target.lanes,
preview.get().accepted_lanes,
preview.get().target_generation);
if (!verified.has_value()) {
return abort_transaction(verified.error());
}
const auto verified_phase = set_phase(P104TransactionPhase::verified);
if (!verified_phase.has_value()) {
return abort_transaction(verified_phase.error());
}
const auto committed_phase = set_phase(P104TransactionPhase::committed);
if (!committed_phase.has_value()) {
return abort_transaction(committed_phase.error());
}
if (request.progress) {
const std::string verified_step = std::string("verified-") +
(preview.get().target_generation == LinkGeneration::gen1
? "gen1x"
: "gen2x") +
std::to_string(verified.get());
request.progress(verified_step.c_str());
}
const auto cleanup = backend->close();
backend.reset();
if (!cleanup.has_value()) {
return Result<P104TransactionJournal>{{cleanup.error()}};
}
return Result<P104TransactionJournal>{std::move(journal)};
}
}
