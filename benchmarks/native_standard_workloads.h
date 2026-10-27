#pragma once

#include <cstdint>
#include <span>

namespace vela::benchmarks {

std::int64_t DynamicValues(std::span<const std::int64_t> input);
std::int64_t DynamicDispatch(std::span<const std::int64_t> input);
std::int64_t JsonRoundtrip(std::span<const std::int64_t> input);
std::int64_t ByteDataLittle(std::span<const std::int64_t> input);
std::int64_t ByteDataBig(std::span<const std::int64_t> input);
std::int64_t RegExpRecords(std::span<const std::int64_t> input);
std::int64_t NumericParse(std::span<const std::int64_t> input);
std::int64_t AsciiCase(std::span<const std::int64_t> input);
std::int64_t UtcRoundtrip(std::span<const std::int64_t> input);

}  // namespace vela::benchmarks
