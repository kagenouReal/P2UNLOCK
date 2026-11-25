#include "pseries/platform/cuda_verifier.hpp"
#include "pseries/platform/profile_store.hpp"
#include "pseries/platform/target_topology.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
namespace pseries::platform {
namespace {
using CUresult = int;
using CUdevice = int;
using CUcontext = void*;
using CUdeviceptr = unsigned long long;
using CuInitFn = CUresult (*)(unsigned int);
using CuDeviceGetByPCIBusIdFn = CUresult (*)(CUdevice*, const char*);
using CuCtxCreateFn = CUresult (*)(CUcontext*, unsigned int, CUdevice);
using CuCtxDestroyFn = CUresult (*)(CUcontext);
using CuCtxSynchronizeFn = CUresult (*)();
using CuMemAllocFn = CUresult (*)(CUdeviceptr*, std::size_t);
using CuMemFreeFn = CUresult (*)(CUdeviceptr);
using CuMemHostAllocFn = CUresult (*)(void**, std::size_t, unsigned int);
using CuMemFreeHostFn = CUresult (*)(void*);
using CuMemcpyHtoDFn = CUresult (*)(CUdeviceptr, const void*, std::size_t);
using CuMemcpyDtoHFn = CUresult (*)(void*, CUdeviceptr, std::size_t);
using CuMemcpyDtoDFn = CUresult (*)(CUdeviceptr, CUdeviceptr, std::size_t);
using CuGetErrorStringFn = CUresult (*)(CUresult, const char**);
constexpr unsigned int kCuMemHostAllocPortable = 0x01u;
Error cuda_error(
CuGetErrorStringFn get_error_string,
CUresult result,
const std::string& operation) {
const char* description = nullptr;
if (get_error_string != nullptr && get_error_string(result, &description) == 0 &&
description != nullptr) {
return Error{
ErrorCode::service_error,
operation + " failed: " + description};
}
return Error{
ErrorCode::service_error,
operation + " failed with CUresult " + std::to_string(result)};
}
double bandwidth_mib_s(std::size_t bytes, double seconds) {
if (seconds <= 0.0) {
return 0.0;
}
return static_cast<double>(bytes) / seconds / (1024.0 * 1024.0);
}
std::wstring wide_text(const std::string& value) {
if (value.empty()) {
return {};
}
const int length = MultiByteToWideChar(
CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
nullptr, 0);
if (length <= 0) {
return {};
}
std::wstring result(static_cast<std::size_t>(length), L'\0');
static_cast<void>(MultiByteToWideChar(
CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
result.data(), length));
return result;
}
std::string cuda_pci_bus_id(const PciBdf& bdf) {
std::ostringstream output;
output << std::hex << std::setfill('0')
<< std::setw(4) << bdf.domain << ':'
<< std::setw(2) << static_cast<unsigned>(bdf.bus) << ':'
<< std::setw(2) << static_cast<unsigned>(bdf.device) << '.'
<< static_cast<unsigned>(bdf.function);
return output.str();
}
}
Result<CudaBandwidth> run_cuda_transfer(
std::size_t buffer_bytes,
int iterations) {
const auto library = LoadLibraryExW(
L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
if (library == nullptr) {
return Result<CudaBandwidth>{{Error{
ErrorCode::hardware_not_found,
"nvcuda.dll is unavailable in System32; NVIDIA driver is not installed"}}};
}
const auto cuInit = reinterpret_cast<CuInitFn>(GetProcAddress(library, "cuInit"));
const auto cuDeviceGetByPCIBusId = reinterpret_cast<CuDeviceGetByPCIBusIdFn>(
GetProcAddress(library, "cuDeviceGetByPCIBusId"));
const auto cuCtxCreate = reinterpret_cast<CuCtxCreateFn>(GetProcAddress(library, "cuCtxCreate"));
const auto cuCtxDestroy = reinterpret_cast<CuCtxDestroyFn>(GetProcAddress(library, "cuCtxDestroy"));
const auto cuCtxSynchronize = reinterpret_cast<CuCtxSynchronizeFn>(
GetProcAddress(library, "cuCtxSynchronize"));
const auto cuMemAlloc = reinterpret_cast<CuMemAllocFn>(GetProcAddress(library, "cuMemAlloc"));
const auto cuMemFree = reinterpret_cast<CuMemFreeFn>(GetProcAddress(library, "cuMemFree"));
const auto cuMemHostAlloc = reinterpret_cast<CuMemHostAllocFn>(
GetProcAddress(library, "cuMemHostAlloc"));
const auto cuMemFreeHost = reinterpret_cast<CuMemFreeHostFn>(
GetProcAddress(library, "cuMemFreeHost"));
const auto cuMemcpyHtoD = reinterpret_cast<CuMemcpyHtoDFn>(GetProcAddress(library, "cuMemcpyHtoD"));
const auto cuMemcpyDtoH = reinterpret_cast<CuMemcpyDtoHFn>(GetProcAddress(library, "cuMemcpyDtoH"));
const auto cuMemcpyDtoD = reinterpret_cast<CuMemcpyDtoDFn>(GetProcAddress(library, "cuMemcpyDtoD"));
const auto cuGetErrorString = reinterpret_cast<CuGetErrorStringFn>(
GetProcAddress(library, "cuGetErrorString"));
if (cuInit == nullptr || cuDeviceGetByPCIBusId == nullptr || cuCtxCreate == nullptr ||
cuCtxDestroy == nullptr || cuCtxSynchronize == nullptr ||
cuMemAlloc == nullptr || cuMemFree == nullptr ||
cuMemHostAlloc == nullptr || cuMemFreeHost == nullptr ||
cuMemcpyHtoD == nullptr || cuMemcpyDtoH == nullptr ||
cuMemcpyDtoD == nullptr) {
static_cast<void>(FreeLibrary(library));
return Result<CudaBandwidth>{{Error{
ErrorCode::not_implemented,
"nvcuda.dll does not expose the required driver API symbols"}}};
}
CUcontext context = nullptr;
CUdeviceptr device_buffer = 0;
CUdeviceptr device_buffer_peer = 0;
unsigned char* original = nullptr;
unsigned char* readback = nullptr;
bool context_created = false;
bool buffer_allocated = false;
bool peer_buffer_allocated = false;
bool original_allocated = false;
bool readback_allocated = false;
auto finish = [&](Result<CudaBandwidth> result) {
if (peer_buffer_allocated) {
static_cast<void>(cuMemFree(device_buffer_peer));
}
if (buffer_allocated) {
static_cast<void>(cuMemFree(device_buffer));
}
if (original_allocated) {
static_cast<void>(cuMemFreeHost(original));
}
if (readback_allocated) {
static_cast<void>(cuMemFreeHost(readback));
}
if (context_created) {
static_cast<void>(cuCtxDestroy(context));
}
static_cast<void>(FreeLibrary(library));
return result;
};
auto result = cuInit(0);
if (result != 0) {
return finish(Result<CudaBandwidth>{{
cuda_error(cuGetErrorString, result, "cuInit")}});
}
std::wstring driver_path = default_driver_path();
const auto profile = load_p104_service_profile();
if (profile.has_value() && !profile.get().driver_path.empty()) {
driver_path = wide_text(profile.get().driver_path);
}
const auto target = resolve_p104_target_topology(driver_path);
if (!target.has_value()) {
return finish(Result<CudaBandwidth>{{Error{
ErrorCode::hardware_not_found,
"CUDA target topology failed: " + target.error().message}}});
}
const auto bus_id = cuda_pci_bus_id(target.get().gpu_bdf);
CUdevice device = 0;
result = cuDeviceGetByPCIBusId(&device, bus_id.c_str());
if (result != 0) {
return finish(Result<CudaBandwidth>{{
cuda_error(
cuGetErrorString, result, "cuDeviceGetByPCIBusId " + bus_id)}});
}
result = cuCtxCreate(&context, 0, device);
if (result != 0) {
return finish(Result<CudaBandwidth>{{
cuda_error(cuGetErrorString, result, "cuCtxCreate")}});
}
context_created = true;
result = cuMemAlloc(&device_buffer, buffer_bytes);
if (result != 0) {
return finish(Result<CudaBandwidth>{{
cuda_error(cuGetErrorString, result, "cuMemAlloc")}});
}
buffer_allocated = true;
result = cuMemAlloc(&device_buffer_peer, buffer_bytes);
if (result != 0) {
return finish(Result<CudaBandwidth>{{
cuda_error(cuGetErrorString, result, "cuMemAlloc peer")}});
}
peer_buffer_allocated = true;
result = cuMemHostAlloc(
reinterpret_cast<void**>(&original), buffer_bytes,
kCuMemHostAllocPortable);
if (result != 0) {
return finish(Result<CudaBandwidth>{{
cuda_error(cuGetErrorString, result, "cuMemHostAlloc original")}});
}
original_allocated = true;
result = cuMemHostAlloc(
reinterpret_cast<void**>(&readback), buffer_bytes,
kCuMemHostAllocPortable);
if (result != 0) {
return finish(Result<CudaBandwidth>{{
cuda_error(cuGetErrorString, result, "cuMemHostAlloc readback")}});
}
readback_allocated = true;
for (std::size_t index = 0; index < buffer_bytes; ++index) {
original[index] = static_cast<unsigned char>((index * 31u + 7u) & 0xFFu);
}
enum class CopyDirection {
host_to_device,
device_to_host,
device_to_device,
peer_to_host
};
auto copy_and_time = [&](CopyDirection direction) -> Result<double> {
const auto start = std::chrono::steady_clock::now();
const char* operation = "CUDA copy";
CUresult copy_result = 0;
switch (direction) {
case CopyDirection::host_to_device:
operation = "cuMemcpyHtoD";
copy_result = cuMemcpyHtoD(device_buffer, original, buffer_bytes);
break;
case CopyDirection::device_to_host:
operation = "cuMemcpyDtoH";
copy_result = cuMemcpyDtoH(readback, device_buffer, buffer_bytes);
break;
case CopyDirection::device_to_device:
operation = "cuMemcpyDtoD";
copy_result = cuMemcpyDtoD(device_buffer_peer, device_buffer, buffer_bytes);
break;
case CopyDirection::peer_to_host:
operation = "cuMemcpyDtoH peer";
copy_result = cuMemcpyDtoH(readback, device_buffer_peer, buffer_bytes);
break;
}
if (copy_result != 0) {
return Result<double>{{cuda_error(cuGetErrorString, copy_result, operation)}};
}
const CUresult sync_result = cuCtxSynchronize();
if (sync_result != 0) {
return Result<double>{{cuda_error(
cuGetErrorString, sync_result, "cuCtxSynchronize")}};
}
const auto end = std::chrono::steady_clock::now();
return Result<double>{std::chrono::duration<double>(end - start).count()};
};
const auto warmup = copy_and_time(CopyDirection::host_to_device);
if (!warmup.has_value()) {
return finish(Result<CudaBandwidth>{{warmup.error()}});
}
const auto warmup_peer = copy_and_time(CopyDirection::device_to_device);
if (!warmup_peer.has_value()) {
return finish(Result<CudaBandwidth>{{warmup_peer.error()}});
}
double best_host_to_device = 0.0;
double best_device_to_host = 0.0;
double best_device_to_device = 0.0;
for (int iteration = 0; iteration < iterations; ++iteration) {
const auto host_to_device = copy_and_time(CopyDirection::host_to_device);
if (!host_to_device.has_value()) {
return finish(Result<CudaBandwidth>{{host_to_device.error()}});
}
if (best_host_to_device == 0.0 ||
host_to_device.get() < best_host_to_device) {
best_host_to_device = host_to_device.get();
}
const auto device_to_host = copy_and_time(CopyDirection::device_to_host);
if (!device_to_host.has_value()) {
return finish(Result<CudaBandwidth>{{device_to_host.error()}});
}
if (best_device_to_host == 0.0 ||
device_to_host.get() < best_device_to_host) {
best_device_to_host = device_to_host.get();
}
if (std::memcmp(original, readback, buffer_bytes) != 0) {
return finish(Result<CudaBandwidth>{{Error{
ErrorCode::service_error,
"CUDA full-buffer copy comparison failed"}}});
}
const auto device_to_device = copy_and_time(CopyDirection::device_to_device);
if (!device_to_device.has_value()) {
return finish(Result<CudaBandwidth>{{device_to_device.error()}});
}
if (best_device_to_device == 0.0 ||
device_to_device.get() < best_device_to_device) {
best_device_to_device = device_to_device.get();
}
}
const auto peer_readback = copy_and_time(CopyDirection::peer_to_host);
if (!peer_readback.has_value()) {
return finish(Result<CudaBandwidth>{{peer_readback.error()}});
}
if (std::memcmp(original, readback, buffer_bytes) != 0) {
return finish(Result<CudaBandwidth>{{Error{
ErrorCode::service_error,
"CUDA device-to-device copy comparison failed"}}});
}
for (std::size_t index = 0; index < buffer_bytes; ++index) {
original[index] = static_cast<unsigned char>(~original[index]);
}
const auto inverse_write = copy_and_time(CopyDirection::host_to_device);
if (!inverse_write.has_value()) {
return finish(Result<CudaBandwidth>{{inverse_write.error()}});
}
const auto inverse_read = copy_and_time(CopyDirection::device_to_host);
if (!inverse_read.has_value()) {
return finish(Result<CudaBandwidth>{{inverse_read.error()}});
}
if (std::memcmp(original, readback, buffer_bytes) != 0) {
return finish(Result<CudaBandwidth>{{Error{
ErrorCode::service_error,
"CUDA inverse-buffer copy comparison failed"}}});
}
CudaBandwidth bandwidth{};
bandwidth.bytes = buffer_bytes;
bandwidth.device_to_host_mib_s = bandwidth_mib_s(buffer_bytes, best_device_to_host);
bandwidth.host_to_device_mib_s = bandwidth_mib_s(buffer_bytes, best_host_to_device);
bandwidth.device_to_device_mib_s = bandwidth_mib_s(buffer_bytes, best_device_to_device);
return finish(Result<CudaBandwidth>{bandwidth});
}
Result<CudaBandwidth> measure_cuda_bandwidth() {
return run_cuda_transfer(64u * 1024u * 1024u, 3);
}
Result<CudaVerification> verify_cuda_full_buffer_and_inverse() {
const auto measured = run_cuda_transfer(4u * 1024u * 1024u, 1);
if (!measured.has_value()) {
return Result<CudaVerification>{{measured.error()}};
}
return Result<CudaVerification>{CudaVerification{measured.get().bytes}};
}
}
