#pragma once

#include <cstdint>
#include <string_view>

namespace vela::runtime {
class NativeContext;
}

namespace vela::benchmarks {

inline constexpr double kNativeDoubleIncrement = 0.5;

void NativeEmpty(runtime::NativeContext& context) noexcept;
std::int64_t NativeInteger(runtime::NativeContext& context, std::int64_t value) noexcept;
double NativeDouble(runtime::NativeContext& context, double value) noexcept;
std::int64_t NativeStringBorrow(runtime::NativeContext& context, std::string_view text) noexcept;

}  // namespace vela::benchmarks
