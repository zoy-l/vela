#include "benchmarks/native_standard_workloads.h"

#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include <absl/strings/numbers.h>
#include <absl/time/civil_time.h>
#include <absl/time/time.h>
#include <pcre2.h>
#include <yyjson.h>

#include "base/json.h"
#include "base/utf8.h"

namespace vela::benchmarks {
namespace {

constexpr std::string_view kJsonText = "Vela😀";
constexpr std::string_view kRegExpRecord = "中=7;😀 ";
constexpr std::string_view kRegExpPattern = "(?<key>中)=([0-9]+)";
constexpr std::int64_t kMillisecondsPerStep = 1001;
constexpr double kJsonNumber = 1.25;
constexpr double kDecimalFraction = 0.25;

std::int64_t ByteData(std::span<const std::int64_t> input, std::endian order) {
  std::vector<std::byte> bytes((input.size() * sizeof(std::int64_t)) + 1);
  const auto view = std::span(bytes).subspan(1);
  for (std::size_t index = 0; index < input.size(); ++index) {
    auto bits = static_cast<std::uint64_t>(input.subspan(index).front());
    if (order != std::endian::native) {
      bits = std::byteswap(bits);
    }
    std::memcpy(view.subspan(index * sizeof(bits)).data(), &bits, sizeof(bits));
  }
  std::int64_t checksum = 0;
  for (std::size_t index = 0; index < input.size(); ++index) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, view.subspan(index * sizeof(bits)).data(), sizeof(bits));
    if (order != std::endian::native) {
      bits = std::byteswap(bits);
    }
    checksum += std::bit_cast<std::int64_t>(bits);
  }
  return checksum + std::to_integer<std::int64_t>(view.last(sizeof(std::int64_t)).front());
}

struct NativeMatch final {
  std::size_t start;
  std::size_t end;
  std::string key;
  std::string number;
};

}  // namespace

std::int64_t DynamicValues(std::span<const std::int64_t> input) {
  using Dynamic = std::variant<std::int64_t, std::string, bool>;
  std::vector<Dynamic> values;
  values.reserve(input.size());
  for (const auto value : input) {
    const auto remainder = value - (value / 3 * 3);
    if (remainder == 0) {
      values.emplace_back(value);
    } else if (remainder == 1) {
      values.emplace_back(std::string("Vela"));
    } else {
      values.emplace_back(true);
    }
  }
  std::int64_t checksum = 0;
  for (const auto& value : values) {
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
      checksum += *integer;
    } else if (const auto* text = std::get_if<std::string>(&value)) {
      checksum += static_cast<std::int64_t>(text->size());
    } else if (std::get<bool>(value)) {
      ++checksum;
    }
  }
  return checksum;
}

std::int64_t DynamicDispatch(std::span<const std::int64_t> input) {
  std::int64_t current = 0;
  const std::function<std::int64_t(std::int64_t)> next = [&current](std::int64_t value) {
    current += value;
    return current;
  };
  std::int64_t checksum = 0;
  for (const auto value : input) {
    checksum += next(value);
  }
  return checksum;
}

std::int64_t JsonRoundtrip(std::span<const std::int64_t> input) {
  JsonOutput output;
  auto* document = output.document();
  auto* records = yyjson_mut_arr(document);
  yyjson_mut_doc_set_root(document, records);
  for (const auto value : input) {
    auto* record = yyjson_mut_obj(document);
    AddJsonField(document, record, "id", yyjson_mut_sint(document, value));
    AddJsonField(document, record, "text",
                 yyjson_mut_strn(document, kJsonText.data(), kJsonText.size()));
    auto* items = yyjson_mut_arr(document);
    yyjson_mut_arr_add_bool(document, items, true);
    yyjson_mut_arr_add_null(document, items);
    yyjson_mut_arr_add_real(document, items, kJsonNumber);
    AddJsonField(document, record, "items", items);
    yyjson_mut_arr_append(records, record);
  }
  const auto encoded = output.Serialize();
  if (!encoded) {
    return -1;
  }
  const auto decoded = JsonDocument::Parse(*encoded);
  if (!decoded || yyjson_arr_size(decoded->root()) != input.size()) {
    return -1;
  }
  auto checksum = static_cast<std::int64_t>(encoded->size());
  auto iterator = yyjson_arr_iter_with(decoded->root());
  for (const auto expected : input) {
    const auto* record = yyjson_arr_iter_next(&iterator);
    const auto* identifier = GetJsonField(record, "id");
    const auto* text = GetJsonField(record, "text");
    const auto* items = GetJsonField(record, "items");
    if (!yyjson_is_int(identifier) || yyjson_get_sint(identifier) != expected ||
        !yyjson_is_str(text) || !yyjson_equals_strn(text, kJsonText.data(), kJsonText.size()) ||
        yyjson_arr_size(items) != 3 || !yyjson_is_true(yyjson_arr_get(items, 0)) ||
        !yyjson_is_null(yyjson_arr_get(items, 1)) ||
        yyjson_get_real(yyjson_arr_get(items, 2)) != kJsonNumber) {
      return -1;
    }
    checksum += expected + static_cast<std::int64_t>(Utf8Length(kJsonText)) + 3;
  }
  return checksum;
}

std::int64_t ByteDataLittle(std::span<const std::int64_t> input) {
  return ByteData(input, std::endian::little);
}

std::int64_t ByteDataBig(std::span<const std::int64_t> input) {
  return ByteData(input, std::endian::big);
}

std::int64_t RegExpRecords(std::span<const std::int64_t> input) {
  std::string text;
  for (const auto value : input) {
    static_cast<void>(value);
    text += kRegExpRecord;
  }
  const std::unique_ptr<pcre2_compile_context, decltype(&pcre2_compile_context_free)> context(
      pcre2_compile_context_create(nullptr), pcre2_compile_context_free);
  pcre2_set_newline(context.get(), PCRE2_NEWLINE_ANY);
  pcre2_set_bsr(context.get(), PCRE2_BSR_UNICODE);
  int error = 0;
  PCRE2_SIZE error_offset = 0;
  const void* pattern_bytes = kRegExpPattern.data();
  const std::unique_ptr<pcre2_code, decltype(&pcre2_code_free)> code(
      pcre2_compile(static_cast<PCRE2_SPTR>(pattern_bytes), kRegExpPattern.size(),
                    PCRE2_UTF | PCRE2_UCP | PCRE2_NEVER_BACKSLASH_C, &error, &error_offset,
                    context.get()),
      pcre2_code_free);
  if (!code) {
    return -1;
  }
  const std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)> data(
      pcre2_match_data_create_from_pattern(code.get(), nullptr), pcre2_match_data_free);
  std::vector<NativeMatch> matches;
  std::size_t cursor = 0;
  std::size_t points = 0;
  const void* subject_bytes = text.data();
  const void* key_name = "key";
  constexpr std::size_t kCaptureOffsets = 6;
  constexpr std::size_t kNumberCapture = 4;
  while (cursor < text.size()) {
    const auto count = pcre2_match(code.get(), static_cast<PCRE2_SPTR>(subject_bytes), text.size(),
                                   cursor, PCRE2_NO_UTF_CHECK, data.get(), nullptr);
    if (count == PCRE2_ERROR_NOMATCH) {
      break;
    }
    if (count < 0) {
      return -1;
    }
    const auto offsets = std::span(pcre2_get_ovector_pointer(data.get()), kCaptureOffsets);
    const auto start = offsets.front();
    const auto end = offsets.subspan(1).front();
    points += Utf8Length(std::string_view(text).substr(cursor, start - cursor));
    const auto point_end = points + Utf8Length(std::string_view(text).substr(start, end - start));
    const auto key = static_cast<std::size_t>(pcre2_substring_number_from_name(
                         code.get(), static_cast<PCRE2_SPTR>(key_name))) *
                     2;
    const auto key_start = offsets.subspan(key).front();
    const auto number_start = offsets.subspan(kNumberCapture).front();
    matches.push_back({
        .start = points,
        .end = point_end,
        .key = text.substr(key_start, offsets.subspan(key + 1).front() - key_start),
        .number =
            text.substr(number_start, offsets.subspan(kNumberCapture + 1).front() - number_start),
    });
    cursor = end;
    points = point_end;
  }
  if (matches.size() != input.size()) {
    return -1;
  }
  std::int64_t checksum = 0;
  for (const auto& match : matches) {
    std::int64_t number = 0;
    const auto* number_end = std::span(match.number).last(0).data();
    const auto parsed = std::from_chars(match.number.data(), number_end, number);
    if (parsed.ec != std::errc() || parsed.ptr != number_end) {
      return -1;
    }
    checksum += static_cast<std::int64_t>(match.start + match.end + Utf8Length(match.key)) + number;
  }
  return checksum;
}

std::int64_t NumericParse(std::span<const std::int64_t> input) {
  std::int64_t checksum = 0;
  for (const auto value : input) {
    const auto text = std::to_string(value);
    std::int64_t integer = 0;
    const auto* text_end = std::span(text).last(0).data();
    const auto parsed = std::from_chars(text.data(), text_end, integer);
    double decimal = 0;
    if (parsed.ec != std::errc() || parsed.ptr != text_end ||
        !absl::SimpleAtod(text + ".25", &decimal) ||
        decimal != static_cast<double>(integer) + kDecimalFraction) {
      return -1;
    }
    checksum += integer + static_cast<std::int64_t>(decimal);
  }
  return checksum;
}

std::int64_t AsciiCase(std::span<const std::int64_t> input) {
  std::int64_t checksum = 0;
  for (const auto value : input) {
    const auto text = "Vela_" + std::to_string(value);
    auto upper = text;
    for (auto& byte : upper) {
      if (byte >= 'a' && byte <= 'z') {
        byte = static_cast<char>(byte - 'a' + 'A');
      }
    }
    auto lower = upper;
    for (auto& byte : lower) {
      if (byte >= 'A' && byte <= 'Z') {
        byte = static_cast<char>(byte - 'A' + 'a');
      }
    }
    if (lower != "vela_" + std::to_string(value)) {
      return -1;
    }
    checksum += static_cast<std::int64_t>(lower.size());
  }
  return checksum;
}

std::int64_t UtcRoundtrip(std::span<const std::int64_t> input) {
  const auto zone = absl::UTCTimeZone();
  const auto base = absl::FromCivil(absl::CivilSecond(2026, 1, 1), zone);
  std::int64_t checksum = 0;
  for (const auto value : input) {
    const auto timestamp = base + absl::Milliseconds(value * kMillisecondsPerStep);
    const auto text = absl::FormatTime("%E4Y-%m-%dT%H:%M:%E3SZ", timestamp, zone);
    absl::Time parsed;
    std::string error;
    if (!absl::ParseTime("%E4Y-%m-%dT%H:%M:%E*SZ", text, zone, &parsed, &error) ||
        parsed != timestamp) {
      return -1;
    }
    const auto civil = absl::ToCivilSecond(parsed, zone);
    const auto millisecond = absl::ToInt64Milliseconds(parsed - absl::FromCivil(civil, zone));
    checksum += absl::ToInt64Milliseconds(parsed - base) + millisecond + civil.second() +
                static_cast<std::int64_t>(text.size());
  }
  return checksum;
}

}  // namespace vela::benchmarks
