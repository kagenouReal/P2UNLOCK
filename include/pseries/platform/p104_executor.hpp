#pragma once
#include "pseries/device_selection.hpp"
#include "pseries/mcfg.hpp"
#include "pseries/p104_plan.hpp"
#include "pseries/platform/pci_config.hpp"
#include "pseries/recovery.hpp"
#include "pseries/types.hpp"
#include <functional>
#include <string>
#include <vector>
namespace pseries::platform {
struct P104UnlockRequest {
std::wstring driver_path;
std::wstring journal_path;
std::wstring gpu_instance_id;
PciBdf gpu_bdf;
PciBdf upstream_bdf;
std::uint16_t gpu_vendor_id{nvidia_vendor_id};
std::uint16_t gpu_device_id{p104_100_device_id};
std::uint16_t upstream_vendor_id{0x8086};
std::uint16_t upstream_device_id{0x0151};
std::vector<McfgAllocation> allocations;
std::uint8_t target_lanes{0};
LinkGeneration target_generation{LinkGeneration::gen2};
bool skip_pnp_disable{false};
std::function<void(const char*)> progress;
};
struct P104PlanPreview {
PciLinkInspection gpu_link;
PciLinkInspection upstream_link;
std::uint64_t bar0_base{};
P104InitialRegisters initial;
P104TargetProfile target;
std::uint8_t accepted_lanes{};
LinkGeneration target_generation{LinkGeneration::gen2};
bool width_change_needed{false};
std::vector<JournalRegisterSnapshot> policy_and_width;
bool already_target{false};
bool target_reached{false};
};
[[nodiscard]] Result<P104PlanPreview> inspect_p104_plan(
const P104UnlockRequest& request);
[[nodiscard]] Result<P104TransactionJournal> execute_p104_unlock(
const P104UnlockRequest& request);
[[nodiscard]] Result<void> reconcile_p104_journal(
const P104UnlockRequest& request);
struct P104DeclarationRequest {
std::wstring driver_path;
PciBdf gpu_bdf;
PciBdf upstream_bdf;
std::uint16_t gpu_vendor_id{nvidia_vendor_id};
std::uint16_t gpu_device_id{p104_100_device_id};
std::uint16_t upstream_vendor_id{};
std::uint16_t upstream_device_id{};
std::vector<McfgAllocation> allocations;
std::function<void(const char*)> progress;
};
struct P104DeclarationStatus {
PciLinkInspection gpu_link;
PciLinkInspection upstream_link;
std::uint64_t bar0_base{};
std::uint32_t link_config{};
std::uint8_t declared_width{};
std::uint8_t negotiated_width{};
bool stable_link{false};
};
[[nodiscard]] Result<P104DeclarationStatus> inspect_p104_declaration(
const P104DeclarationRequest& request);
[[nodiscard]] Result<P104DeclarationStatus> execute_p104_decorative_x16(
const P104DeclarationRequest& request);
[[nodiscard]] Result<P104DeclarationStatus> execute_p104_declaration_negotiated(
const P104DeclarationRequest& request);
[[nodiscard]] Result<void> hold_p104_decorative_x16(
const P104DeclarationRequest& request);
}
