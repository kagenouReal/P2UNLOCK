#include "pseries/device_selection.hpp"
#include "pseries/platform/cuda_verifier.hpp"
#include "pseries/platform/p104_executor.hpp"
#include "pseries/platform/pci_config.hpp"
#include "pseries/platform/profile_store.hpp"
#include "pseries/platform/seavo_backend.hpp"
#include "pseries/platform/service_host.hpp"
#include "pseries/platform/service_install.hpp"
#include "pseries/platform/service_ipc.hpp"
#include "pseries/platform/target_topology.hpp"
#include "pseries/platform/transaction_journal_store.hpp"
#include "pseries/profile.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
namespace {
const wchar_t p104_journal_path[] = L"C:\\ProgramData\\P2UNLOCK\\p104.state";
const wchar_t* model_name(std::uint16_t device_id) {
switch (device_id) {
case pseries::p104_100_device_id:
return L"P104-100";
case pseries::p104_101_device_id:
return L"P104-101";
default:
return L"Unsupported GPU";
}
}
const wchar_t* generation_name(pseries::LinkGeneration generation) {
switch (generation) {
case pseries::LinkGeneration::gen1:
return L"Gen1";
case pseries::LinkGeneration::gen2:
return L"Gen2";
case pseries::LinkGeneration::gen3:
return L"Gen3";
case pseries::LinkGeneration::gen4:
return L"Gen4";
case pseries::LinkGeneration::gen5:
return L"Gen5";
default:
return L"Unknown";
}
}
std::string narrow_text(const std::wstring& value) {
return std::string(value.begin(), value.end());
}
std::wstring wide_text(const std::string& value) {
return std::wstring(value.begin(), value.end());
}
std::wstring bdf_text(const pseries::PciBdf& bdf) {
std::wostringstream stream;
stream << std::hex << std::uppercase << std::setfill(L'0')
<< std::setw(4) << bdf.domain << L":"
<< std::setw(2) << static_cast<unsigned>(bdf.bus) << L":"
<< std::setw(2) << static_cast<unsigned>(bdf.device) << L"."
<< std::setw(1) << static_cast<unsigned>(bdf.function);
return stream.str();
}
void print_help() {
std::wcout
<< L"P2UNLOCK - P104-100/P104-101 unlock and verification tool\n\n"
<< L"Commands:\n"
<< L"  install              register the auto-start service\n"
<< L"  uninstall            remove service and saved profile\n"
<< L"  scan                 read-only topology and link inspection\n"
<< L"  status               show saved profile and declared/negotiated width\n"
<< L"  width 8|16           unlock lane count (x8 or x16)\n"
<< L"  gen 1|2              unlock link speed (Gen1 or Gen2)\n"
<< L"  revert               clear profile and restore negotiated declaration\n"
<< L"  verify               CUDA full-buffer and inverse check\n"
<< L"  bw                   CUDA bandwidth (pinned host, P104 BDF)\n\n"
<< L"Notes:\n"
<< L"  install registers the service only; use width/gen to set the profile.\n"
<< L"  Example: install -> width 8 -> gen 2.\n"
<< L"  width changes lanes; gen changes speed. Run both if needed.\n"
<< L"  uninstall requires a PC restart to return fully to stock.\n";
}
void print_link_inspection(
const wchar_t* label,
const pseries::PciBdf& bdf,
const pseries::platform::PciLinkInspection& inspection) {
std::wcout << label << L" [" << std::hex << std::uppercase << std::setfill(L'0')
<< std::setw(4) << inspection.vendor_id << L":"
<< std::setw(4) << inspection.device_id << L"]"
<< L" BDF " << bdf_text(bdf) << std::dec << L"\n"
<< L"  Maximum: "
<< generation_name(*inspection.link.maximum_generation)
<< L" x" << static_cast<unsigned>(inspection.link.maximum_width->lanes)
<< L"; negotiated: "
<< generation_name(inspection.link.current_generation)
<< L" x" << static_cast<unsigned>(inspection.link.negotiated_width.lanes)
<< L"; training: "
<< (inspection.link.training_state == pseries::LinkTrainingState::training
? L"active"
: L"clear")
<< L"\n";
}
int run_scan() {
const auto driver = pseries::platform::default_driver_path();
const auto target = pseries::platform::resolve_p104_target_topology(driver);
if (!target.has_value()) {
std::cerr << "Scan failed: " << target.error().message << '\n';
return 1;
}
const auto loaded = pseries::platform::SeavoBackend::load(driver);
if (!loaded.has_value()) {
std::cerr << "Driver load failed: " << loaded.error().message << '\n';
return 1;
}
const auto gpu = pseries::platform::inspect_pcie_link(
*loaded.get(), target.get().allocations, target.get().gpu_bdf);
const auto upstream = pseries::platform::inspect_pcie_link(
*loaded.get(), target.get().allocations, target.get().upstream_bdf);
static_cast<void>(loaded.get()->close());
if (!gpu.has_value() || !upstream.has_value()) {
std::cerr << "Link inspection failed\n";
return 1;
}
std::wcout << L"Read-only scan. No configuration writes were performed.\n"
<< L"GPU: " << model_name(target.get().gpu_device_id)
<< L" [" << std::hex << std::uppercase
<< target.get().gpu_vendor_id << L":"
<< target.get().gpu_device_id << std::dec << L"]\n";
print_link_inspection(
model_name(target.get().gpu_device_id), target.get().gpu_bdf, gpu.get());
print_link_inspection(L"Upstream bridge", target.get().upstream_bdf, upstream.get());
std::wcout << L"Width unlocks: x8/x16; speed unlock: Gen2. "
L"Both declare x16 to reporting tools.\n";
return 0;
}
int run_verify() {
const auto driver = pseries::platform::default_driver_path();
const auto target = pseries::platform::resolve_p104_target_topology(driver);
if (!target.has_value()) {
std::cerr << "Verify failed: " << target.error().message << '\n';
return 1;
}
const auto loaded = pseries::platform::SeavoBackend::load(driver);
if (!loaded.has_value()) {
std::cerr << "Driver load failed: " << loaded.error().message << '\n';
return 1;
}
const auto gpu = pseries::platform::inspect_pcie_link(
*loaded.get(), target.get().allocations, target.get().gpu_bdf);
const auto upstream = pseries::platform::inspect_pcie_link(
*loaded.get(), target.get().allocations, target.get().upstream_bdf);
static_cast<void>(loaded.get()->close());
if (!gpu.has_value() || !upstream.has_value()) {
std::cerr << "Link inspection failed\n";
return 1;
}
const auto generation = gpu.get().link.current_generation;
if ((generation != pseries::LinkGeneration::gen1 &&
generation != pseries::LinkGeneration::gen2) ||
upstream.get().link.current_generation != generation ||
gpu.get().link.negotiated_width.lanes !=
upstream.get().link.negotiated_width.lanes ||
gpu.get().link.training_state != pseries::LinkTrainingState::stable ||
upstream.get().link.training_state != pseries::LinkTrainingState::stable) {
std::cerr << "Both ends must be at a stable matching Gen1 or Gen2 link\n";
return 1;
}
const auto cuda = pseries::platform::verify_cuda_full_buffer_and_inverse();
if (!cuda.has_value()) {
std::cerr << "CUDA verification failed: " << cuda.error().message << '\n';
return 1;
}
std::wcout << L"VERIFIED: "
<< generation_name(generation)
<< L" x"
<< static_cast<unsigned>(gpu.get().link.negotiated_width.lanes)
<< L"; declared x"
<< static_cast<unsigned>(gpu.get().link.maximum_width.has_value()
? gpu.get().link.maximum_width->lanes
: 0)
<< L"; CUDA full buffer and inverse passed ("
<< cuda.get().bytes << L" bytes).\n";
return 0;
}
int run_direct_status() {
const auto profile = pseries::platform::load_p104_service_profile();
const auto driver = profile.has_value() &&
!profile.get().driver_path.empty()
? wide_text(profile.get().driver_path)
: pseries::platform::default_driver_path();
const auto target = pseries::platform::resolve_p104_target_topology(driver);
if (!target.has_value()) {
std::cerr << "Status failed: " << target.error().message << '\n';
return 1;
}
pseries::platform::P104DeclarationRequest request{};
request.driver_path = driver;
request.gpu_bdf = target.get().gpu_bdf;
request.upstream_bdf = target.get().upstream_bdf;
request.gpu_vendor_id = target.get().gpu_vendor_id;
request.gpu_device_id = target.get().gpu_device_id;
request.upstream_vendor_id = target.get().upstream_vendor_id;
request.upstream_device_id = target.get().upstream_device_id;
request.allocations = target.get().allocations;
const auto status = pseries::platform::inspect_p104_declaration(request);
if (!status.has_value()) {
std::cerr << "Status failed: " << status.error().message << '\n';
return 1;
}
std::cout << "OK profile="
<< (profile.has_value()
? pseries::p104_profile_name(profile.get().profile)
: "none")
<< " declared=" << static_cast<unsigned>(status.get().declared_width)
<< " negotiated=" << static_cast<unsigned>(status.get().negotiated_width)
<< '\n';
return 0;
}
int run_status() {
std::string response;
const auto sent = pseries::platform::send_service_command("STATUS", response);
if (sent.has_value()) {
std::cout << response << '\n';
return 0;
}
return run_direct_status();
}
int run_bandwidth() {
std::string response;
if (pseries::platform::send_service_command("BW", response).has_value()) {
std::cout << response << '\n';
return 0;
}
const auto bandwidth = pseries::platform::measure_cuda_bandwidth();
if (!bandwidth.has_value()) {
std::cerr << "Bandwidth check failed: " << bandwidth.error().message << '\n';
return 1;
}
std::cout << "OK bytes=" << bandwidth.get().bytes
<< " device_to_host_mib_s=" << std::fixed << std::setprecision(2)
<< bandwidth.get().device_to_host_mib_s
<< " host_to_device_mib_s="
<< bandwidth.get().host_to_device_mib_s
<< " device_to_device_mib_s="
<< bandwidth.get().device_to_device_mib_s << '\n';
return 0;
}
int run_width(const std::string& value) {
unsigned lanes = 0;
if (value == "8") {
lanes = 8;
} else if (value == "16") {
lanes = 16;
} else {
std::cerr << "Width must be 8 or 16 lanes\n";
return 2;
}
std::string response;
if (pseries::platform::send_service_command("WIDTH " + value, response).has_value()) {
std::cout << response << '\n';
return 0;
}
const auto applied = pseries::platform::apply_p104_width(
static_cast<std::uint8_t>(lanes));
if (!applied.has_value()) {
std::cerr << "Width change failed: " << applied.error().message << '\n';
return 1;
}
std::wcout << L"Width set to x" << lanes
<< L"; profile saved with cosmetic x16 declaration.\n";
return 0;
}
int run_gen(const std::string& value) {
pseries::LinkGeneration generation{};
if (value == "1" || value == "gen1" || value == "Gen1") {
generation = pseries::LinkGeneration::gen1;
} else if (value == "2" || value == "gen2" || value == "Gen2") {
generation = pseries::LinkGeneration::gen2;
} else {
std::cerr << "Generation must be 1 or 2\n";
return 2;
}
const std::string request =
generation == pseries::LinkGeneration::gen1 ? "GEN 1" : "GEN 2";
std::string response;
if (pseries::platform::send_service_command(request, response).has_value()) {
std::cout << response << '\n';
return 0;
}
const auto applied = pseries::platform::apply_p104_generation(generation);
if (!applied.has_value()) {
std::cerr << "Generation change failed: " << applied.error().message << '\n';
return 1;
}
const auto profile = pseries::platform::load_p104_service_profile();
if (profile.has_value()) {
std::wcout << L"Generation set to "
<< generation_name(generation)
<< L"; profile saved with cosmetic x16 declaration.\n";
} else {
std::wcout << L"Generation set to "
<< generation_name(generation)
<< L" at stock x4; profile cleared and declaration "
L"restored.\n";
}
return 0;
}
int run_install() {
static_cast<void>(pseries::platform::stop_p2unlock_service());
wchar_t path[MAX_PATH]{};
const auto length = GetModuleFileNameW(nullptr, path, MAX_PATH);
if (length == 0 || length == MAX_PATH) {
std::cerr << "Could not resolve the executable path\n";
return 1;
}
const auto installed = pseries::platform::install_p2unlock_service(
std::wstring(path, length), pseries::platform::default_driver_path());
if (!installed.has_value()) {
std::cerr << "Install failed: " << installed.error().message << '\n';
return 1;
}
const auto started = pseries::platform::start_p2unlock_service();
if (!started.has_value()) {
std::cerr << "Service installed but could not be started: "
<< started.error().message << '\n';
return 1;
}
std::wcout << L"P2UNLOCK service installed and started.\n";
return 0;
}
int run_uninstall() {
static_cast<void>(pseries::platform::stop_p2unlock_service());
const auto removed = pseries::platform::uninstall_p2unlock_service();
if (!removed.has_value()) {
std::cerr << "Uninstall failed: " << removed.error().message << '\n';
return 1;
}
static_cast<void>(pseries::platform::clear_p104_service_profile());
const auto journal_cleared =
pseries::platform::clear_transaction_journal(p104_journal_path);
if (!journal_cleared.has_value()) {
std::cerr << "Warning: transaction journal was not cleared: "
<< journal_cleared.error().message << '\n';
}
std::wcout << L"P2UNLOCK service removed and saved profile cleared.\n"
<< L"Restart the PC if you want to return fully to stock Gen1 x4.\n";
return 0;
}
int run_revert() {
std::string response;
const auto sent = pseries::platform::send_service_command("REVERT", response);
if (sent.has_value()) {
std::cout << response << '\n';
return 0;
}
static_cast<void>(pseries::platform::clear_p104_service_profile());
const auto request_driver = pseries::platform::default_driver_path();
const auto target = pseries::platform::resolve_p104_target_topology(request_driver);
if (!target.has_value()) {
std::cerr << "Revert failed: " << target.error().message << '\n';
return 1;
}
pseries::platform::P104DeclarationRequest request{};
request.driver_path = request_driver;
request.gpu_bdf = target.get().gpu_bdf;
request.upstream_bdf = target.get().upstream_bdf;
request.gpu_vendor_id = target.get().gpu_vendor_id;
request.gpu_device_id = target.get().gpu_device_id;
request.upstream_vendor_id = target.get().upstream_vendor_id;
request.upstream_device_id = target.get().upstream_device_id;
request.allocations = target.get().allocations;
const auto restored = pseries::platform::execute_p104_declaration_negotiated(request);
if (!restored.has_value()) {
std::cerr << "Revert failed: " << restored.error().message << '\n';
return 1;
}
const auto journal_cleared =
pseries::platform::clear_transaction_journal(p104_journal_path);
if (!journal_cleared.has_value()) {
std::cerr << "Revert failed: " << journal_cleared.error().message << '\n';
return 1;
}
std::wcout << L"Saved profile cleared and declaration restored to the "
L"negotiated width.\n";
return 0;
}
}
int wmain(int argc, wchar_t* argv[]) {
if (argc == 2 && std::wstring_view(argv[1]) == L"--service") {
const auto result = pseries::platform::run_p2unlock_service();
if (!result.has_value()) {
std::cerr << result.error().message << '\n';
return 1;
}
return 0;
}
if (argc == 1 || (argc == 2 && std::wstring_view(argv[1]) == L"help")) {
print_help();
return 0;
}
const std::wstring_view command(argv[1]);
if (command == L"install" && argc == 2) {
return run_install();
}
if (command == L"uninstall" && argc == 2) {
return run_uninstall();
}
if (command == L"scan" && argc == 2) {
return run_scan();
}
if (command == L"verify" && argc == 2) {
return run_verify();
}
if (command == L"bw" && argc == 2) {
return run_bandwidth();
}
if (command == L"status" && argc == 2) {
return run_status();
}
if (command == L"revert" && argc == 2) {
return run_revert();
}
if (command == L"width" && argc == 3) {
return run_width(narrow_text(argv[2]));
}
if (command == L"gen" && argc == 3) {
return run_gen(narrow_text(argv[2]));
}
std::wcerr << L"Unsupported command. Run `P2UNLOCK.exe help`.\n";
return 2;
}
