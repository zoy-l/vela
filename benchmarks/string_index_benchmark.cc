#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "benchmark/benchmark.h"

#include "base/utf8.h"
#include "runtime/heap/heap.h"
#include "runtime/strings/string.h"

namespace vela::benchmarks {
namespace {

enum class TextEncoding : std::uint8_t { Ascii, Mixed };
enum class LengthMethod : std::uint8_t { Scan, Cached };
enum class IndexMethod : std::uint8_t { FromStart, Forward, Reverse };
constexpr std::size_t kPointsPerBlock = 4;

std::string MakeText(std::size_t blocks, TextEncoding encoding) {
  const std::string_view block = encoding == TextEncoding::Ascii ? "abcd" : "A中😀e";
  std::string text;
  text.reserve(blocks * block.size());
  for (std::size_t index = 0; index < blocks; ++index) {
    text += block;
  }
  return text;
}

template <TextEncoding Encoding, LengthMethod Method>
void StringLength(benchmark::State& state) {
  const auto blocks = static_cast<std::size_t>(state.range(0));
  const auto text = MakeText(blocks, Encoding);
  runtime::Heap heap;
  const auto* string = heap.AllocateString(text);
  if (string == nullptr) {
    state.SkipWithError("String allocation failed");
    return;
  }
  for (auto iteration : state) {
    std::size_t count = 0;
    if constexpr (Method == LengthMethod::Scan) {
      count = Utf8Length(string->value());
    } else {
      benchmark::DoNotOptimize(string);
      count = string->length();
    }
    benchmark::DoNotOptimize(count);
    if (count != blocks * kPointsPerBlock) {
      state.SkipWithError("Incorrect code point count");
      break;
    }
  }
}

template <TextEncoding Encoding, IndexMethod Method>
void StringIndex(benchmark::State& state) {
  const auto text = MakeText(static_cast<std::size_t>(state.range(0)), Encoding);
  std::size_t expected = 0;
  for (std::size_t offset = 0; offset < text.size(); ++offset) {
    if ((static_cast<unsigned char>(text.at(offset)) & kContinuationMask) != kContinuationTag) {
      expected += offset;
    }
  }
  runtime::Heap heap;
  const auto* string = heap.AllocateString(text);
  if (string == nullptr) {
    state.SkipWithError("String allocation failed");
    return;
  }
  const auto count = string->length();
  for (auto iteration : state) {
    std::size_t sum = 0;
    for (std::uint32_t step = 0; step < count; ++step) {
      const auto index = Method == IndexMethod::Reverse ? count - step - 1 : step;
      if constexpr (Method == IndexMethod::FromStart) {
        sum += *Utf8Offset(string->value(), index);
      } else {
        sum += string->ByteOffset(index);
      }
    }
    benchmark::DoNotOptimize(sum);
    if (sum != expected) {
      state.SkipWithError("Incorrect code point offsets");
      break;
    }
  }
  state.SetItemsProcessed(state.iterations() * count);
}

BENCHMARK_TEMPLATE(StringLength, TextEncoding::Ascii, LengthMethod::Scan)
    ->Name("string/length_scan_ascii")
    ->Arg(256)
    ->Arg(1024);
BENCHMARK_TEMPLATE(StringLength, TextEncoding::Ascii, LengthMethod::Cached)
    ->Name("string/length_cached_ascii")
    ->Arg(256)
    ->Arg(1024);
BENCHMARK_TEMPLATE(StringLength, TextEncoding::Mixed, LengthMethod::Scan)
    ->Name("string/length_scan_mixed")
    ->Arg(256)
    ->Arg(1024);
BENCHMARK_TEMPLATE(StringLength, TextEncoding::Mixed, LengthMethod::Cached)
    ->Name("string/length_cached_mixed")
    ->Arg(256)
    ->Arg(1024);
BENCHMARK_TEMPLATE(StringIndex, TextEncoding::Ascii, IndexMethod::FromStart)
    ->Name("string/index_from_start_ascii")
    ->Arg(256)
    ->Arg(1024);
BENCHMARK_TEMPLATE(StringIndex, TextEncoding::Ascii, IndexMethod::Forward)
    ->Name("string/index_cursor_forward_ascii")
    ->Arg(256)
    ->Arg(1024);
BENCHMARK_TEMPLATE(StringIndex, TextEncoding::Ascii, IndexMethod::Reverse)
    ->Name("string/index_cursor_reverse_ascii")
    ->Arg(256)
    ->Arg(1024);
BENCHMARK_TEMPLATE(StringIndex, TextEncoding::Mixed, IndexMethod::FromStart)
    ->Name("string/index_from_start_mixed")
    ->Arg(256)
    ->Arg(1024);
BENCHMARK_TEMPLATE(StringIndex, TextEncoding::Mixed, IndexMethod::Forward)
    ->Name("string/index_cursor_forward_mixed")
    ->Arg(256)
    ->Arg(1024);
BENCHMARK_TEMPLATE(StringIndex, TextEncoding::Mixed, IndexMethod::Reverse)
    ->Name("string/index_cursor_reverse_mixed")
    ->Arg(256)
    ->Arg(1024);

}  // namespace
}  // namespace vela::benchmarks
