#include "benchmarks/native_call_kernels.h"

#include <cstdint>
#include <string_view>

#include "benchmark/benchmark.h"

namespace vela::benchmarks {

// 独立翻译单元并禁止内联，让直接调用基线保留真实调用边界。
[[clang::noinline]] void NativeEmpty(runtime::NativeContext& /*context*/) noexcept {
  // 编译器屏障没有运行时业务工作，防止空调用在跨过程优化时消失。
  benchmark::ClobberMemory();
}

[[clang::noinline]] std::int64_t NativeInteger(runtime::NativeContext& /*context*/,
                                               std::int64_t value) noexcept {
  return value + 1;
}

[[clang::noinline]] double NativeDouble(runtime::NativeContext& /*context*/,
                                        double value) noexcept {
  return value + kNativeDoubleIncrement;
}

[[clang::noinline]] std::int64_t NativeStringBorrow(runtime::NativeContext& /*context*/,
                                                    std::string_view text) noexcept {
  return static_cast<std::int64_t>(text.size()) + static_cast<unsigned char>(text.front());
}

}  // namespace vela::benchmarks
