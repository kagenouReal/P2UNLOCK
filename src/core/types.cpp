#include "pseries/types.hpp"
namespace pseries {
bool PciBdf::is_valid() const noexcept {
return device <= 0x1fu && function <= 0x07u;
}
}
