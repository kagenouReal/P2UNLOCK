#include "pseries/platform/target_topology.hpp"
#include "pseries/device_selection.hpp"
#include "pseries/platform/firmware_table.hpp"
#include "pseries/platform/seavo_backend.hpp"
namespace pseries::platform {
Result<TargetTopology> resolve_p104_target_topology(
const std::wstring& driver_path) {
if (driver_path.empty()) {
return Result<TargetTopology>{{Error{
ErrorCode::invalid_argument,
"Topology discovery requires a driver path"}}};
}
const auto discovered = enumerate_present_pci_devices();
if (!discovered.has_value()) {
return Result<TargetTopology>{{Error{
ErrorCode::hardware_not_found,
"PCI discovery failed: " + discovered.error().message}}};
}
const PciDeviceRecord* gpu = nullptr;
std::size_t gpu_count = 0;
for (const auto& device : discovered.get()) {
if (pseries::is_supported_unlock_gpu(
device.identity.vendor_id, device.identity.device_id)) {
gpu = &device;
++gpu_count;
}
}
if (gpu_count != 1) {
return Result<TargetTopology>{{Error{
ErrorCode::hardware_not_found,
"Exactly one supported P104 GPU (P104-100 or P104-101) is required"}}};
}
const auto allocations = read_system_mcfg();
if (!allocations.has_value()) {
return Result<TargetTopology>{{allocations.error()}};
}
const auto gpu_bdf = resolve_mcfg_bdf(
allocations.get(), gpu->bus_address);
if (!gpu_bdf.has_value()) {
return Result<TargetTopology>{{gpu_bdf.error()}};
}
const auto loaded = SeavoBackend::load(driver_path);
if (!loaded.has_value()) {
return Result<TargetTopology>{{loaded.error()}};
}
const auto& backend = loaded.get();
const auto gpu_link = inspect_pcie_link(
*backend, allocations.get(), gpu_bdf.get());
if (!gpu_link.has_value()) {
static_cast<void>(backend->close());
return Result<TargetTopology>{{gpu_link.error()}};
}
if (gpu_link.get().vendor_id != gpu->identity.vendor_id ||
gpu_link.get().device_id != gpu->identity.device_id) {
static_cast<void>(backend->close());
return Result<TargetTopology>{{Error{
ErrorCode::hardware_not_found,
"P104 config-space identity does not match Windows PnP"}}};
}
const auto bridge = find_upstream_pcie_bridge(
*backend, allocations.get(), gpu_bdf.get());
if (!bridge.has_value()) {
static_cast<void>(backend->close());
return Result<TargetTopology>{{bridge.error()}};
}
if (bridge.get().bdf.domain != gpu_bdf.get().domain) {
static_cast<void>(backend->close());
return Result<TargetTopology>{{Error{
ErrorCode::unsupported,
"P104 and upstream bridge resolved to different PCI segments"}}};
}
const auto upstream_link = inspect_pcie_link(
*backend, allocations.get(), bridge.get().bdf);
if (!upstream_link.has_value()) {
static_cast<void>(backend->close());
return Result<TargetTopology>{{upstream_link.error()}};
}
if (upstream_link.get().vendor_id != bridge.get().vendor_id ||
upstream_link.get().device_id != bridge.get().device_id) {
static_cast<void>(backend->close());
return Result<TargetTopology>{{Error{
ErrorCode::hardware_not_found,
"Upstream bridge identity changed during inspection"}}};
}
const auto cleanup = backend->close();
if (!cleanup.has_value()) {
return Result<TargetTopology>{{cleanup.error()}};
}
TargetTopology target{};
target.gpu = *gpu;
target.upstream.identity.vendor_id = bridge.get().vendor_id;
target.upstream.identity.device_id = bridge.get().device_id;
target.upstream.bus_address.bus = bridge.get().bdf.bus;
target.upstream.bus_address.device = bridge.get().bdf.device;
target.upstream.bus_address.function = bridge.get().bdf.function;
target.gpu_bdf = gpu_bdf.get();
target.upstream_bdf = bridge.get().bdf;
target.gpu_vendor_id = gpu_link.get().vendor_id;
target.gpu_device_id = gpu_link.get().device_id;
target.upstream_vendor_id = upstream_link.get().vendor_id;
target.upstream_device_id = upstream_link.get().device_id;
target.allocations = allocations.get();
return Result<TargetTopology>{target};
}
}
