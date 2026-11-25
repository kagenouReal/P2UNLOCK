#pragma once
#include "pseries/types.hpp"
#include <atomic>
#include <functional>
#include <string>
#include <string_view>
namespace pseries::platform {
[[nodiscard]] Result<void> send_service_command(
std::string_view command,
std::string& response);
[[nodiscard]] Result<void> serve_service_commands(
const std::atomic_bool& stop,
const std::function<std::string(std::string)>& handler);
}
