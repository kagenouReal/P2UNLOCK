#include "pseries/platform/service_host.hpp"
#include "pseries/platform/cuda_verifier.hpp"
#include "pseries/platform/p104_executor.hpp"
#include "pseries/platform/pci_config.hpp"
#include "pseries/platform/profile_store.hpp"
#include "pseries/platform/service_install.hpp"
#include "pseries/platform/service_ipc.hpp"
#include "pseries/platform/seavo_backend.hpp"
#include "pseries/platform/target_topology.hpp"
#include "pseries/platform/transaction_journal_store.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
namespace pseries::platform {
namespace {
std::mutex g_seavo_mutex;
std::string g_last_profile_key;
std::string g_last_attempted_key;
constexpr DWORD kWatchdogInitialBackoffMs = 1000;
constexpr DWORD kWatchdogMaxBackoffMs = 30000;
DWORD g_watchdog_backoff_ms = kWatchdogInitialBackoffMs;
std::chrono::steady_clock::time_point g_watchdog_next_attempt{};
const wchar_t p104_journal_path[] = L"C:\\ProgramData\\P2UNLOCK\\p104.state";
void reset_watchdog_retry() noexcept {
g_watchdog_backoff_ms = kWatchdogInitialBackoffMs;
g_watchdog_next_attempt = std::chrono::steady_clock::now();
}
void schedule_watchdog_retry() noexcept {
g_watchdog_next_attempt = std::chrono::steady_clock::now() +
std::chrono::milliseconds(g_watchdog_backoff_ms);
if (g_watchdog_backoff_ms < kWatchdogMaxBackoffMs / 2) {
g_watchdog_backoff_ms *= 2;
} else {
g_watchdog_backoff_ms = kWatchdogMaxBackoffMs;
}
}
bool watchdog_retry_ready() noexcept {
return std::chrono::steady_clock::now() >= g_watchdog_next_attempt;
}
std::string profile_key(const P104ServiceProfile& profile) {
const auto serialized = serialize_p104_service_profile(profile);
return serialized.has_value() ? serialized.get() : std::string{};
}
std::string narrow_text(const std::wstring& value) {
const int length = WideCharToMultiByte(
CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
nullptr, 0, nullptr, nullptr);
if (length <= 0) {
return {};
}
std::string result(static_cast<std::size_t>(length), '\0');
WideCharToMultiByte(
CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
result.data(), length, nullptr, nullptr);
return result;
}
std::wstring wide_text(const std::string& value) {
const int length = MultiByteToWideChar(
CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
nullptr, 0);
if (length <= 0) {
return {};
}
std::wstring result(static_cast<std::size_t>(length), L'\0');
MultiByteToWideChar(
CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
result.data(), length);
return result;
}
std::wstring driver_for_profile(const P104ServiceProfile& profile) {
if (profile.driver_path.empty()) {
return default_driver_path();
}
return wide_text(profile.driver_path);
}
Result<P104DeclarationRequest> declaration_request_for_driver(
const std::wstring& driver_path) {
const auto target = resolve_p104_target_topology(driver_path);
if (!target.has_value()) {
return Result<P104DeclarationRequest>{{target.error()}};
}
P104DeclarationRequest request{};
request.driver_path = driver_path;
request.gpu_bdf = target.get().gpu_bdf;
request.upstream_bdf = target.get().upstream_bdf;
request.gpu_vendor_id = target.get().gpu_vendor_id;
request.gpu_device_id = target.get().gpu_device_id;
request.upstream_vendor_id = target.get().upstream_vendor_id;
request.upstream_device_id = target.get().upstream_device_id;
request.allocations = target.get().allocations;
return Result<P104DeclarationRequest>{request};
}
std::string declared_width_text() {
const auto request = declaration_request_for_driver(default_driver_path());
if (!request.has_value()) {
return "unknown";
}
const auto status = inspect_p104_declaration(request.get());
if (!status.has_value()) {
return "unknown";
}
return std::to_string(status.get().declared_width);
}
Result<P104Profile> detect_current_profile_for_driver(
const std::wstring& driver_path) {
const auto target = resolve_p104_target_topology(driver_path);
if (!target.has_value()) {
return Result<P104Profile>{{target.error()}};
}
const auto loaded = SeavoBackend::load(driver_path);
if (!loaded.has_value()) {
return Result<P104Profile>{{loaded.error()}};
}
const auto gpu = inspect_pcie_link(
*loaded.get(), target.get().allocations, target.get().gpu_bdf);
const auto upstream = inspect_pcie_link(
*loaded.get(), target.get().allocations, target.get().upstream_bdf);
static_cast<void>(loaded.get()->close());
if (!gpu.has_value() || !upstream.has_value()) {
return Result<P104Profile>{{Error{
ErrorCode::hardware_not_found,
"Could not read the P104 and upstream PCIe links"}}};
}
const auto generation = gpu.get().link.current_generation;
const auto lanes = gpu.get().link.negotiated_width.lanes;
if ((generation != LinkGeneration::gen1 &&
generation != LinkGeneration::gen2) ||
upstream.get().link.current_generation != generation ||
upstream.get().link.negotiated_width.lanes != lanes ||
gpu.get().link.training_state != LinkTrainingState::stable ||
upstream.get().link.training_state != LinkTrainingState::stable) {
return Result<P104Profile>{{Error{
ErrorCode::invalid_recovery_state,
"P104 and upstream must be at a stable matching Gen1 or Gen2 link"}}};
}
return make_p104_profile(generation, lanes);
}
std::string service_error(const char* operation, const Error& error) {
return std::string(operation) + ": " + error.message;
}
std::string handle_service_command(const std::string& command) {
std::istringstream stream(command);
std::string verb;
stream >> verb;
if (verb == "PING") {
return "OK pong";
}
if (verb == "PROFILE") {
const auto profile = load_p104_service_profile();
if (!profile.has_value()) {
return "OK no-profile";
}
return std::string("OK profile=") + p104_profile_name(profile.get().profile) +
" driver=" + (profile.get().driver_path.empty()
? narrow_text(default_driver_path())
: profile.get().driver_path);
}
std::lock_guard<std::mutex> guard(g_seavo_mutex);
if (verb == "SCAN") {
const auto driver = default_driver_path();
const auto target = resolve_p104_target_topology(driver);
if (!target.has_value()) {
return "ERR " + target.error().message;
}
const auto loaded = SeavoBackend::load(driver);
if (!loaded.has_value()) {
return "ERR " + loaded.error().message;
}
const auto gpu = inspect_pcie_link(
*loaded.get(), target.get().allocations, target.get().gpu_bdf);
static_cast<void>(loaded.get()->close());
if (!gpu.has_value()) {
return "ERR " + gpu.error().message;
}
return std::string("OK gpu=10DE:1B87 upstream=") +
std::to_string(target.get().upstream_vendor_id) + ":" +
std::to_string(target.get().upstream_device_id) +
" maxwidth=" + std::to_string(gpu.get().link.maximum_width.has_value()
? gpu.get().link.maximum_width->lanes
: 0) +
" negotiated=" + std::to_string(gpu.get().link.negotiated_width.lanes);
}
if (verb == "STATUS") {
const auto profile = load_p104_service_profile();
const auto driver = profile.has_value()
? driver_for_profile(profile.get())
: default_driver_path();
const auto request = declaration_request_for_driver(driver);
if (!request.has_value()) {
return "ERR " + request.error().message;
}
const auto status = inspect_p104_declaration(request.get());
if (!status.has_value()) {
return "ERR " + status.error().message;
}
return std::string("OK profile=") +
(profile.has_value() ? p104_profile_name(profile.get().profile) : "none") +
" declared=" + std::to_string(status.get().declared_width) +
" negotiated=" + std::to_string(status.get().negotiated_width);
}
if (verb == "VERIFY") {
const auto cuda = verify_cuda_full_buffer_and_inverse();
if (!cuda.has_value()) {
return "ERR " + cuda.error().message;
}
return std::string("OK bytes=") + std::to_string(cuda.get().bytes);
}
if (verb == "BW") {
const auto bandwidth = measure_cuda_bandwidth();
if (!bandwidth.has_value()) {
return "ERR " + bandwidth.error().message;
}
std::ostringstream output;
output << "OK bytes=" << bandwidth.get().bytes
<< " device_to_host_mib_s=" << std::fixed << std::setprecision(2)
<< bandwidth.get().device_to_host_mib_s
<< " host_to_device_mib_s="
<< bandwidth.get().host_to_device_mib_s
<< " device_to_device_mib_s="
<< bandwidth.get().device_to_device_mib_s;
return output.str();
}
if (verb == "WIDTH") {
std::string value;
stream >> value;
unsigned lanes = 0;
if (value == "8") {
lanes = 8;
} else if (value == "16") {
lanes = 16;
} else {
return "ERR width must be 8 or 16";
}
const auto applied = apply_p104_width(static_cast<std::uint8_t>(lanes));
const auto profile = load_p104_service_profile();
const auto key = profile.has_value() ? profile_key(profile.get()) : std::string{};
g_last_attempted_key = key;
g_last_profile_key = applied.has_value() ? key : std::string{};
if (applied.has_value()) {
reset_watchdog_retry();
} else {
reset_watchdog_retry();
schedule_watchdog_retry();
}
if (!applied.has_value()) {
return service_error("apply width", applied.error());
}
return std::string("OK width=") + value +
" profile=" + (profile.has_value()
? p104_profile_name(profile.get().profile)
: "none") +
" declared=" + declared_width_text();
}
if (verb == "GEN") {
std::string value;
stream >> value;
LinkGeneration generation{};
if (value == "1" || value == "gen1" || value == "Gen1") {
generation = LinkGeneration::gen1;
} else if (value == "2" || value == "gen2" || value == "Gen2") {
generation = LinkGeneration::gen2;
} else {
return "ERR generation must be 1 or 2";
}
const auto applied = apply_p104_generation(generation);
const auto profile = load_p104_service_profile();
const auto key = profile.has_value() ? profile_key(profile.get()) : std::string{};
g_last_attempted_key = key;
g_last_profile_key = applied.has_value() ? key : std::string{};
if (applied.has_value()) {
reset_watchdog_retry();
} else {
reset_watchdog_retry();
schedule_watchdog_retry();
}
if (!applied.has_value()) {
return service_error("apply generation", applied.error());
}
return std::string("OK gen=") +
(generation == LinkGeneration::gen1 ? "1" : "2") +
" profile=" + (profile.has_value()
? p104_profile_name(profile.get().profile)
: "none") +
" declared=" + declared_width_text();
}
if (verb == "REVERT") {
const auto cleared = clear_p104_service_profile();
if (!cleared.has_value()) {
return service_error("clear profile", cleared.error());
}
const auto request = declaration_request_for_driver(default_driver_path());
if (!request.has_value()) {
return "ERR " + request.error().message;
}
const auto restored = execute_p104_declaration_negotiated(request.get());
if (!restored.has_value()) {
return service_error("restore declaration", restored.error());
}
const auto journal_cleared = clear_transaction_journal(p104_journal_path);
if (!journal_cleared.has_value()) {
return service_error("clear journal", journal_cleared.error());
}
g_last_profile_key.clear();
g_last_attempted_key.clear();
reset_watchdog_retry();
return "OK profile cleared declared=negotiated";
}
return "ERR unknown-command";
}
SERVICE_STATUS g_status{};
SERVICE_STATUS_HANDLE g_status_handle = nullptr;
HANDLE g_stop_event = nullptr;
HANDLE g_watchdog_thread = nullptr;
HANDLE g_ipc_thread = nullptr;
std::atomic_bool g_service_stop{false};
void set_status(DWORD state, DWORD win32_exit_code = NO_ERROR) {
g_status.dwCurrentState = state;
g_status.dwWin32ExitCode = win32_exit_code;
if (state == SERVICE_RUNNING) {
g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
g_status.dwCheckPoint = 0;
g_status.dwWaitHint = 0;
} else if (state == SERVICE_STOPPED) {
g_status.dwControlsAccepted = 0;
g_status.dwCheckPoint = 0;
g_status.dwWaitHint = 0;
} else {
g_status.dwControlsAccepted = 0;
++g_status.dwCheckPoint;
g_status.dwWaitHint = 5000;
}
if (g_status_handle != nullptr) {
SetServiceStatus(g_status_handle, &g_status);
}
}
DWORD WINAPI service_control(DWORD control, DWORD, LPVOID, LPVOID) {
if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
g_service_stop.store(true);
set_status(SERVICE_STOP_PENDING);
if (g_stop_event != nullptr) {
SetEvent(g_stop_event);
}
return NO_ERROR;
}
if (control == SERVICE_CONTROL_INTERROGATE) {
SetServiceStatus(g_status_handle, &g_status);
}
return NO_ERROR;
}
DWORD WINAPI watchdog_worker(void*) {
while (!g_service_stop.load()) {
{
std::lock_guard<std::mutex> guard(g_seavo_mutex);
const auto profile = load_p104_service_profile();
if (profile.has_value()) {
const auto key = profile_key(profile.get());
if (key != g_last_attempted_key) {
g_last_attempted_key = key;
g_last_profile_key.clear();
reset_watchdog_retry();
}
if (key != g_last_profile_key && watchdog_retry_ready()) {
const auto applied = apply_p104_service_profile(profile.get());
if (applied.has_value()) {
g_last_profile_key = key;
reset_watchdog_retry();
} else {
g_last_profile_key.clear();
schedule_watchdog_retry();
}
}
} else {
g_last_profile_key.clear();
g_last_attempted_key.clear();
reset_watchdog_retry();
}
}
if (WaitForSingleObject(g_stop_event, 1000) == WAIT_OBJECT_0) {
break;
}
}
return 0;
}
DWORD WINAPI ipc_worker(void*) {
static_cast<void>(serve_service_commands(g_service_stop, handle_service_command));
return 0;
}
void WINAPI service_main(DWORD, LPWSTR*) {
g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
g_status.dwControlsAccepted = 0;
g_status.dwCurrentState = SERVICE_START_PENDING;
g_status.dwWin32ExitCode = NO_ERROR;
g_status.dwCheckPoint = 0;
g_status.dwWaitHint = 5000;
g_status_handle = RegisterServiceCtrlHandlerExW(
kP2UnlockServiceName, service_control, nullptr);
if (g_status_handle == nullptr) {
return;
}
set_status(SERVICE_START_PENDING);
g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
if (g_stop_event == nullptr) {
set_status(SERVICE_STOPPED, GetLastError());
return;
}
g_watchdog_thread = CreateThread(nullptr, 0, watchdog_worker, nullptr, 0, nullptr);
g_ipc_thread = CreateThread(nullptr, 0, ipc_worker, nullptr, 0, nullptr);
if (g_watchdog_thread == nullptr || g_ipc_thread == nullptr) {
g_service_stop.store(true);
SetEvent(g_stop_event);
set_status(SERVICE_STOPPED, GetLastError());
return;
}
set_status(SERVICE_RUNNING);
WaitForSingleObject(g_watchdog_thread, INFINITE);
g_service_stop.store(true);
if (g_stop_event != nullptr) {
SetEvent(g_stop_event);
}
WaitForSingleObject(g_ipc_thread, INFINITE);
CloseHandle(g_watchdog_thread);
CloseHandle(g_ipc_thread);
CloseHandle(g_stop_event);
g_watchdog_thread = nullptr;
g_ipc_thread = nullptr;
g_stop_event = nullptr;
set_status(SERVICE_STOPPED);
}
}
Result<void> apply_p104_service_profile(const P104ServiceProfile& profile) {
const auto lanes = p104_profile_lanes(profile.profile);
if (!lanes.has_value()) {
return Result<void>{{lanes.error()}};
}
const auto generation = p104_profile_generation(profile.profile);
if (!generation.has_value()) {
return Result<void>{{generation.error()}};
}
const auto driver_path = driver_for_profile(profile);
const auto target = resolve_p104_target_topology(driver_path);
if (!target.has_value()) {
return Result<void>{{target.error()}};
}
const auto declaration = declaration_request_for_driver(driver_path);
if (!declaration.has_value()) {
return Result<void>{{declaration.error()}};
}
const auto preselect = execute_p104_decorative_x16(declaration.get());
if (!preselect.has_value()) {
return Result<void>{{preselect.error()}};
}
P104UnlockRequest unlock{};
unlock.driver_path = driver_path;
unlock.journal_path = p104_journal_path;
unlock.gpu_instance_id = target.get().gpu.instance_id;
unlock.gpu_bdf = target.get().gpu_bdf;
unlock.upstream_bdf = target.get().upstream_bdf;
unlock.gpu_vendor_id = target.get().gpu_vendor_id;
unlock.gpu_device_id = target.get().gpu_device_id;
unlock.upstream_vendor_id = target.get().upstream_vendor_id;
unlock.upstream_device_id = target.get().upstream_device_id;
unlock.allocations = target.get().allocations;
unlock.target_lanes = lanes.get();
unlock.target_generation = generation.get();
const auto preview = inspect_p104_plan(unlock);
if (!preview.has_value()) {
return Result<void>{{preview.error()}};
}
if (!preview.get().already_target) {
const auto committed = execute_p104_unlock(unlock);
if (!committed.has_value()) {
return Result<void>{{committed.error()}};
}
} else {
const auto reconciled = reconcile_p104_journal(unlock);
if (!reconciled.has_value()) {
return Result<void>{{reconciled.error()}};
}
}
const auto applied = execute_p104_decorative_x16(declaration.get());
if (!applied.has_value()) {
return Result<void>{{applied.error()}};
}
return Result<void>{};
}
Result<P104Profile> detect_current_p104_profile() {
return detect_current_profile_for_driver(default_driver_path());
}
Result<void> apply_p104_width(std::uint8_t lanes) {
const auto current = detect_current_p104_profile();
if (!current.has_value()) {
return Result<void>{{current.error()}};
}
const auto generation = p104_profile_generation(current.get());
if (!generation.has_value()) {
return Result<void>{{generation.error()}};
}
const auto selected = make_p104_profile(generation.get(), lanes);
if (!selected.has_value()) {
return Result<void>{{selected.error()}};
}
P104ServiceProfile profile{};
profile.profile = selected.get();
profile.driver_path = narrow_text(default_driver_path());
const auto saved = save_p104_service_profile(profile);
if (!saved.has_value()) {
return Result<void>{{saved.error()}};
}
return apply_p104_service_profile(profile);
}
Result<void> apply_p104_generation(LinkGeneration generation) {
const auto current = detect_current_p104_profile();
if (!current.has_value()) {
return Result<void>{{current.error()}};
}
const auto lanes = p104_profile_lanes(current.get());
if (!lanes.has_value()) {
return Result<void>{{lanes.error()}};
}
if (generation == LinkGeneration::gen1 && lanes.get() == 4) {
if (current.get() != P104Profile::gen1_4x) {
P104ServiceProfile temporary{};
temporary.profile = P104Profile::gen1_4x;
temporary.driver_path = narrow_text(default_driver_path());
const auto changed = apply_p104_service_profile(temporary);
if (!changed.has_value()) {
return Result<void>{{changed.error()}};
}
}
const auto cleared = clear_p104_service_profile();
if (!cleared.has_value()) {
return Result<void>{{cleared.error()}};
}
const auto request = declaration_request_for_driver(default_driver_path());
if (!request.has_value()) {
return Result<void>{{request.error()}};
}
const auto restored = execute_p104_declaration_negotiated(request.get());
if (!restored.has_value()) {
return Result<void>{{restored.error()}};
}
const auto journal_cleared = clear_transaction_journal(p104_journal_path);
if (!journal_cleared.has_value()) {
return Result<void>{{journal_cleared.error()}};
}
return Result<void>{};
}
const auto selected = make_p104_profile(generation, lanes.get());
if (!selected.has_value()) {
return Result<void>{{selected.error()}};
}
P104ServiceProfile profile{};
profile.profile = selected.get();
profile.driver_path = narrow_text(default_driver_path());
const auto saved = save_p104_service_profile(profile);
if (!saved.has_value()) {
return Result<void>{{saved.error()}};
}
return apply_p104_service_profile(profile);
}
Result<void> run_p2unlock_service() {
wchar_t service_name[] = L"P2UNLOCK";
SERVICE_TABLE_ENTRYW table[] = {
{service_name, service_main},
{nullptr, nullptr},
};
if (!StartServiceCtrlDispatcherW(table)) {
return Result<void>{{Error{
ErrorCode::service_error,
"StartServiceCtrlDispatcherW failed with Win32 error " +
std::to_string(GetLastError())}}};
}
return Result<void>{};
}
}
