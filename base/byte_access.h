#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "base/builtin.h"

namespace vela {

enum class ByteOrder : std::uint8_t { Big, Little, Host };
enum class ByteNumber : std::uint8_t { Signed, Unsigned, Float };

inline constexpr std::uint32_t kBitsPerByte = 8;
inline constexpr std::int64_t kMaximumByte = 255;

struct ByteAccess final {
  std::string_view getter;
  std::string_view setter;
  Builtin get;
  Builtin set;
  std::size_t width;
  ByteNumber number;
};

inline constexpr auto kByteAccesses = std::to_array<ByteAccess>({
    {
        .getter = "getInt8",
        .setter = "setInt8",
        .get = Builtin::ByteDataGetInt8,
        .set = Builtin::ByteDataSetInt8,
        .width = 1,
        .number = ByteNumber::Signed,
    },
    {
        .getter = "getUint8",
        .setter = "setUint8",
        .get = Builtin::ByteDataGetUint8,
        .set = Builtin::ByteDataSetUint8,
        .width = 1,
        .number = ByteNumber::Unsigned,
    },
    {
        .getter = "getInt16",
        .setter = "setInt16",
        .get = Builtin::ByteDataGetInt16,
        .set = Builtin::ByteDataSetInt16,
        .width = 2,
        .number = ByteNumber::Signed,
    },
    {
        .getter = "getUint16",
        .setter = "setUint16",
        .get = Builtin::ByteDataGetUint16,
        .set = Builtin::ByteDataSetUint16,
        .width = 2,
        .number = ByteNumber::Unsigned,
    },
    {
        .getter = "getInt32",
        .setter = "setInt32",
        .get = Builtin::ByteDataGetInt32,
        .set = Builtin::ByteDataSetInt32,
        .width = 4,
        .number = ByteNumber::Signed,
    },
    {
        .getter = "getUint32",
        .setter = "setUint32",
        .get = Builtin::ByteDataGetUint32,
        .set = Builtin::ByteDataSetUint32,
        .width = 4,
        .number = ByteNumber::Unsigned,
    },
    {
        .getter = "getInt64",
        .setter = "setInt64",
        .get = Builtin::ByteDataGetInt64,
        .set = Builtin::ByteDataSetInt64,
        .width = 8,
        .number = ByteNumber::Signed,
    },
    {
        .getter = "getFloat32",
        .setter = "setFloat32",
        .get = Builtin::ByteDataGetFloat32,
        .set = Builtin::ByteDataSetFloat32,
        .width = 4,
        .number = ByteNumber::Float,
    },
    {
        .getter = "getFloat64",
        .setter = "setFloat64",
        .get = Builtin::ByteDataGetFloat64,
        .set = Builtin::ByteDataSetFloat64,
        .width = 8,
        .number = ByteNumber::Float,
    },
});

[[nodiscard]] constexpr bool IsByteBuiltin(Builtin builtin) noexcept {
  return builtin >= Builtin::NewByteArray && builtin <= Builtin::ByteDataSetFloat64;
}

[[nodiscard]] constexpr const ByteAccess* FindByteAccess(Builtin builtin) noexcept {
  for (const auto& access : kByteAccesses) {
    if (access.get == builtin || access.set == builtin) {
      return &access;
    }
  }
  return nullptr;
}

}  // namespace vela
