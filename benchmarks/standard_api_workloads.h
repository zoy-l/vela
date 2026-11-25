#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace vela::benchmarks {

std::int64_t MathAtan2(std::span<const std::int64_t> input);
std::int64_t MathCbrt(std::span<const std::int64_t> input);
std::int64_t MathHypot(std::span<const std::int64_t> input);
std::int64_t MathRemainder(std::span<const std::int64_t> input);
std::int64_t StringBoundary(std::span<const std::int64_t> input);
std::int64_t StringSliceBytes(std::span<const std::int64_t> input);
std::int64_t StringReplaceBytes(std::span<const std::int64_t> input);
std::int64_t StringByteOffset(std::span<const std::int64_t> input);
std::int64_t StringTrim(std::span<const std::int64_t> input);

struct WorkloadProblem final {
  std::int64_t size;
  std::int64_t expected;
};

struct StandardApiWorkload final {
  std::string_view name;
  decltype(&MathAtan2) native;
  std::array<WorkloadProblem, 2> problems;
};

// AOT 和 JIT 使用同一负载、规模及校验值，输入准备均不计入执行时间。
inline constexpr auto kStandardApiWorkloads = std::to_array<StandardApiWorkload>({
    {
        .name = "math_atan2",
        .native = MathAtan2,
        .problems = {{{.size = 256, .expected = 388447}, {.size = 2048, .expected = 3229401}}},
    },
    {
        .name = "math_cbrt",
        .native = MathCbrt,
        .problems = {{{.size = 256, .expected = -5040}, {.size = 2048, .expected = -39946}}},
    },
    {
        .name = "math_hypot",
        .native = MathHypot,
        .problems = {{{.size = 256, .expected = 15512871}, {.size = 2048, .expected = 128374666}}},
    },
    {
        .name = "math_remainder",
        .native = MathRemainder,
        .problems = {{{.size = 256, .expected = -105000}, {.size = 2048, .expected = -105000}}},
    },
    {
        .name = "string_boundary",
        .native = StringBoundary,
        .problems = {{{.size = 256, .expected = 136}, {.size = 2048, .expected = 1091}}},
    },
    {
        .name = "string_slice_bytes",
        .native = StringSliceBytes,
        .problems = {{{.size = 256, .expected = 10240}, {.size = 2048, .expected = 81920}}},
    },
    {
        .name = "string_replace_bytes",
        .native = StringReplaceBytes,
        .problems = {{{.size = 256, .expected = 20608}, {.size = 2048, .expected = 164864}}},
    },
    {
        .name = "string_byte_offset",
        .native = StringByteOffset,
        .problems = {{{.size = 256, .expected = 6422}, {.size = 2048, .expected = 52880}}},
    },
    {
        .name = "string_trim",
        .native = StringTrim,
        .problems = {{{.size = 256, .expected = 13680}, {.size = 2048, .expected = 109280}}},
    },
});

}  // namespace vela::benchmarks
