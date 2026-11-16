#include "runtime/native/native_host_target.h"

#include <array>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#if defined(_WIN32)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace vela::runtime {
namespace {

#if defined(__x86_64__) || defined(_M_X64)
std::array<std::uint32_t, 4> ReadCpuId(std::uint32_t leaf, std::uint32_t subleaf = 0) noexcept {
#if defined(_WIN32)
  std::array<int, 4> registers;
  __cpuidex(registers.data(), static_cast<int>(leaf), static_cast<int>(subleaf));
  return {static_cast<std::uint32_t>(registers[0]), static_cast<std::uint32_t>(registers[1]),
          static_cast<std::uint32_t>(registers[2]), static_cast<std::uint32_t>(registers[3])};
#else
  std::array<std::uint32_t, 4> registers;
  __cpuid_count(leaf, subleaf, registers[0], registers[1], registers[2], registers[3]);
  return registers;
#endif
}

__attribute__((target("xsave"))) std::uint64_t ReadXcr0() noexcept {
  return __builtin_ia32_xgetbv(0);
}

NativeHostTarget DetectHostTarget() noexcept {
  if (ReadCpuId(0)[0] < 7 || ReadCpuId(0x80000000U)[0] < 0x80000001U) {
    return NativeHostTarget::Generic;
  }
  // x86-64-v2/v3 的 SSE3、SSSE3、FMA、CX16、SSE4、MOVBE、POPCNT、
  // XSAVE/OSXSAVE、AVX 与 F16C；不能只检查 AVX512F 就采用完整 v4 目标。
  constexpr auto kBasic = (1U << 0) | (1U << 9) | (1U << 12) | (1U << 13) | (1U << 19) |
                          (1U << 20) | (1U << 22) | (1U << 23) | (1U << 26) | (1U << 27) |
                          (1U << 28) | (1U << 29);
  if ((ReadCpuId(1)[2] & kBasic) != kBasic) {
    return NativeHostTarget::Generic;
  }
  // BMI1、AVX2、BMI2，以及 v4 要求的 AVX512F/DQ/CD/BW/VL。
  constexpr auto kExtended = (1U << 3) | (1U << 5) | (1U << 8) | (1U << 16) | (1U << 17) |
                             (1U << 28) | (1U << 30) | (1U << 31);
  constexpr auto kLongMode = (1U << 0) | (1U << 5);  // LAHF/SAHF 与 LZCNT。
  if ((ReadCpuId(7)[1] & kExtended) != kExtended ||
      (ReadCpuId(0x80000001U)[2] & kLongMode) != kLongMode) {
    return NativeHostTarget::Generic;
  }
  // XMM、YMM、opmask、ZMM 高半部及额外 ZMM 寄存器均须由操作系统启用。
  constexpr std::uint64_t kVectorState = 0xe6;
  return (ReadXcr0() & kVectorState) == kVectorState ? NativeHostTarget::X86_64V4
                                                     : NativeHostTarget::Generic;
}
#endif

}  // namespace

NativeHostTarget CurrentNativeHostTarget() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  static const auto target = DetectHostTarget();
  return target;
#else
  return NativeHostTarget::Generic;
#endif
}

}  // namespace vela::runtime
