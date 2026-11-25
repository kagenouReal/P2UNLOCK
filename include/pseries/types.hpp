#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
namespace pseries {
struct PciBdf {
std::uint16_t domain{};
std::uint8_t bus{};
std::uint8_t device{};
std::uint8_t function{};
[[nodiscard]] bool is_valid() const noexcept;
};
struct PciBusAddress {
std::uint8_t bus{};
std::uint8_t device{};
std::uint8_t function{};
};
struct HardwareIdentity {
std::uint16_t vendor_id{};
std::uint16_t device_id{};
std::optional<std::uint16_t> subsystem_vendor_id;
std::optional<std::uint16_t> subsystem_device_id;
std::optional<PciBdf> bdf;
};
enum class LinkGeneration : std::uint8_t {
unknown,
gen1,
gen2,
gen3,
gen4,
gen5,
};
enum class LinkTrainingState : std::uint8_t {
unknown,
training,
stable,
};
struct LinkWidth {
std::uint8_t lanes{};
};
struct PcieLinkInformation {
LinkGeneration current_generation{LinkGeneration::unknown};
LinkWidth negotiated_width{};
std::optional<LinkGeneration> maximum_generation;
std::optional<LinkWidth> maximum_width;
LinkTrainingState training_state{LinkTrainingState::unknown};
std::uint16_t capability_offset{};
std::uint16_t link_control_offset{};
std::uint16_t link_control_2_offset{};
};
enum class ErrorCode {
none,
invalid_argument,
invalid_configuration,
invalid_recovery_state,
unsupported,
not_implemented,
hardware_not_found,
service_error,
};
struct Error {
ErrorCode code{ErrorCode::none};
std::string message;
};
template<typename T>
struct Result {
std::variant<T, Error> value;
[[nodiscard]] bool has_value() const noexcept {
return std::holds_alternative<T>(value);
}
[[nodiscard]] const T& get() const {
return std::get<T>(value);
}
[[nodiscard]] T& get() {
return std::get<T>(value);
}
[[nodiscard]] const Error& error() const {
return std::get<Error>(value);
}
};
template<>
struct Result<void> {
std::optional<Error> error_value;
[[nodiscard]] bool has_value() const noexcept {
return !error_value.has_value();
}
[[nodiscard]] const Error& error() const {
return *error_value;
}
};
}
