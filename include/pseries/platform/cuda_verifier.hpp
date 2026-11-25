#pragma once
#include "pseries/types.hpp"
#include <cstddef>
namespace pseries::platform {
struct CudaVerification {
std::size_t bytes{};
};
struct CudaBandwidth {
std::size_t bytes{};
double device_to_host_mib_s{};
double host_to_device_mib_s{};
double device_to_device_mib_s{};
};
[[nodiscard]] Result<CudaVerification> verify_cuda_full_buffer_and_inverse();
[[nodiscard]] Result<CudaBandwidth> measure_cuda_bandwidth();
}
