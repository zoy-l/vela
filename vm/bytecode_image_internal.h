#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "base/binary.h"
#include "runtime/value/object.h"

namespace vela::vm::internal {

enum class StandardClass : std::uint8_t { None, Error, DateParts };

inline constexpr auto kNoImageId = std::numeric_limits<std::uint64_t>::max();
inline constexpr std::string_view kImageMagic = "VELABC01";
inline constexpr std::size_t kElementBytes = 3 + (5 * sizeof(std::uint64_t));

[[nodiscard]] std::expected<bool, BinaryError> ReadFlag(BinaryReader& reader);

struct ImageParameterFlags final {
  bool optional = false;
  bool rest = false;
};

[[nodiscard]] std::expected<ImageParameterFlags, BinaryError> ReadParameterFlags(
    BinaryReader& reader);

template <class T>
std::expected<T*, std::string> ResolvePointer(std::uint64_t index, std::span<T* const> values) {
  if (index == kNoImageId) {
    return nullptr;
  }
  if (index >= values.size()) {
    return std::unexpected("invalid layout reference");
  }
  auto* value = values.subspan(static_cast<std::size_t>(index)).front();
  if (value == nullptr) {
    return std::unexpected("invalid layout reference");
  }
  return value;
}

template <class T, class RecordReader>
std::expected<std::vector<T>, BinaryError> ReadRecords(BinaryReader& reader,
                                                       std::size_t minimum_bytes,
                                                       RecordReader read_record) {
  const auto count = reader.Count(minimum_bytes);
  if (!count) {
    return std::unexpected(count.error());
  }
  std::vector<T> records;
  records.reserve(*count);
  for (std::size_t index = 0; index < *count; ++index) {
    auto record = read_record(reader);
    if (!record) {
      return std::unexpected(record.error());
    }
    records.push_back(std::move(*record));
  }
  return records;
}

template <class T>
using ImageUnsigned = std::conditional_t<
    sizeof(T) == 1,
    std::uint8_t,
    std::conditional_t<sizeof(T) == 2,
                       std::uint16_t,
                       std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>>>;

template <class T>
void WriteNumbers(BinaryWriter& writer, std::span<const T> values) {
  writer.Write(static_cast<std::uint64_t>(values.size()));
  for (const auto value : values) {
    writer.Write(std::bit_cast<ImageUnsigned<T>>(value));
  }
}

template <class T>
std::expected<std::vector<T>, BinaryError> ReadNumbers(BinaryReader& reader) {
  const auto count = reader.Read<std::uint64_t>();
  if (!count) {
    return std::unexpected(count.error());
  }
  if (*count > reader.remaining() / sizeof(T)) {
    return std::unexpected(reader.Error("numeric pool length exceeds remaining input"));
  }
  const auto bytes = reader.Take(static_cast<std::size_t>(*count) * sizeof(T));
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  std::vector<T> result(static_cast<std::size_t>(*count));
  constexpr unsigned kByteBits = 8;
  for (std::size_t index = 0; index < result.size(); ++index) {
    ImageUnsigned<T> value = 0;
    for (std::size_t byte = 0; byte < sizeof(T); ++byte) {
      value |= static_cast<ImageUnsigned<T>>(std::to_integer<std::uint8_t>(
                   bytes->subspan((index * sizeof(T)) + byte).front()))
               << (byte * kByteBits);
    }
    result.at(index) = std::bit_cast<T>(value);
  }
  return result;
}

struct ImageElement final {
  std::uint8_t kind = 0;
  std::uint8_t object_kind = 0;
  std::uint8_t flags = 0;
  std::uint64_t interface = kNoImageId;
  std::uint64_t instance = kNoImageId;
  std::uint64_t array = kNoImageId;
  std::uint64_t collection = kNoImageId;
  std::uint64_t signature = kNoImageId;
};

struct ImageCollection final {
  ImageElement keys;
  std::optional<ImageElement> values;
};

struct ImageSignature final {
  struct Parameter final {
    ImageElement type;
    bool optional = false;
    bool rest = false;
  };
  std::vector<Parameter> parameters;
  std::optional<ImageElement> result = std::nullopt;
};

struct LoadedLayouts final {
  std::vector<const runtime::InterfaceType*> interfaces;
  std::vector<runtime::ClassLayout*> classes;
  std::vector<const runtime::ArrayLayout*> arrays;
  std::vector<const runtime::CollectionLayout*> collections;
  std::vector<const runtime::TypeSignature*> signatures;
};

inline constexpr std::uint8_t kNullableImageFlag = 1;
inline constexpr std::uint8_t kCallableImageFlag = 2;
inline constexpr std::uint8_t kInterfaceImageFlag = 4;
inline constexpr std::uint8_t kDynamicImageFlag = 8;

[[nodiscard]] std::expected<ImageElement, BinaryError> ReadElement(BinaryReader& reader);
[[nodiscard]] std::expected<std::vector<std::string>, BinaryError> ReadTexts(BinaryReader& reader);
[[nodiscard]] std::expected<LoadedLayouts, BinaryError> ReadLayouts(BinaryReader& reader,
                                                                    runtime::Isolate& isolate);
[[nodiscard]] std::expected<runtime::ArrayLayout, std::string> ResolveElement(
    const ImageElement& input,
    const LoadedLayouts& layouts);
[[nodiscard]] std::expected<runtime::TypeConstraint, std::string> ResolveType(
    const ImageElement& input,
    const LoadedLayouts& layouts);

}  // namespace vela::vm::internal
