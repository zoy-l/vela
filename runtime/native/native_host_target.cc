#include "runtime/native/native_host_target.h"

#include "runtime/native/native_image.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <array>
#include <cstdint>

#ifdef _WIN32
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace vela::runtime {
namespace {

#if defined(__x86_64__) || defined(_M_X64)
std::array<std::uint32_t, 4> ReadCpuId(std::uint32_t leaf, std::uint32_t subleaf = 0) noexcept {
#ifdef _WIN32
  std::array<int, 4> registers{};
  __cpuidex(registers.data(), static_cast<int>(leaf), static_cast<int>(subleaf));
  return {
      static_cast<std::uint32_t>(registers[0]),
      static_cast<std::uint32_t>(registers[1]),
      static_cast<std::uint32_t>(registers[2]),
      static_cast<std::uint32_t>(registers[3]),
  };
#else
  std::array<std::uint32_t, 4> registers{};
  __cpuid_count(leaf, subleaf, registers[0], registers[1], registers[2], registers[3]);
  return registers;
#endif
}

__attribute__((target("xsave"))) std::uint64_t ReadXcr0() noexcept {
  return __builtin_ia32_xgetbv(0);
}

NativeHostTarget DetectHostTarget() noexcept {
  constexpr std::uint32_t kStructuredFeaturesLeaf = 7;
  constexpr std::uint32_t kExtendedLeafBase = 0x80000000U;
  constexpr std::uint32_t kExtendedFeaturesLeaf = 0x80000001U;
  if (ReadCpuId(0)[0] < kStructuredFeaturesLeaf ||
      ReadCpuId(kExtendedLeafBase)[0] < kExtendedFeaturesLeaf) {
    return NativeHostTarget::Generic;
  }
  // x86-64-v2/v3 的 SSE3、SSSE3、FMA、CX16、SSE4、MOVBE、POPCNT、
  // XSAVE/OSXSAVE、AVX 与 F16C；不能只检查 AVX512F 就采用完整 v4 目标。
  constexpr auto kBasic = (1U << 0U) | (1U << 9U) | (1U << 12U) | (1U << 13U) | (1U << 19U) |
                          (1U << 20U) | (1U << 22U) | (1U << 23U) | (1U << 26U) | (1U << 27U) |
                          (1U << 28U) | (1U << 29U);
  if ((ReadCpuId(1)[2] & kBasic) != kBasic) {
    return NativeHostTarget::Generic;
  }
  // BMI1、AVX2、BMI2，以及 v4 要求的 AVX512F/DQ/CD/BW/VL。
  constexpr auto kExtended = (1U << 3U) | (1U << 5U) | (1U << 8U) | (1U << 16U) | (1U << 17U) |
                             (1U << 28U) | (1U << 30U) | (1U << 31U);
  constexpr auto kLongMode = (1U << 0U) | (1U << 5U);  // LAHF/SAHF 与 LZCNT。
  if ((ReadCpuId(kStructuredFeaturesLeaf)[1] & kExtended) != kExtended ||
      (ReadCpuId(kExtendedFeaturesLeaf)[2] & kLongMode) != kLongMode) {
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
