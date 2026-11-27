#pragma once

#include <array>
#include <string_view>

#include "base/type_kind.h"

namespace vela::frontend {

inline constexpr auto kNullTypeName = BuiltinTypeName(LanguageTypeKind::Null);
inline constexpr auto kVoidTypeName = BuiltinTypeName(LanguageTypeKind::Void);
inline constexpr auto kNeverTypeName = BuiltinTypeName(LanguageTypeKind::Never);
inline constexpr auto kDynamicTypeName = BuiltinTypeName(LanguageTypeKind::Dynamic);
inline constexpr auto kBoolTypeName = BuiltinTypeName(LanguageTypeKind::Bool);
inline constexpr auto kIntTypeName = BuiltinTypeName(LanguageTypeKind::Int);
inline constexpr auto kDoubleTypeName = BuiltinTypeName(LanguageTypeKind::Double);
inline constexpr auto kStringTypeName = BuiltinTypeName(LanguageTypeKind::String);
inline constexpr std::string_view kIntArrayTypeName = "int[]";
inline constexpr std::string_view kStringArrayTypeName = "string[]";
inline constexpr std::string_view kNullableStringTypeName = "string?";
inline constexpr auto kMutableStringTypeName = BuiltinTypeName(LanguageTypeKind::MutableString);
inline constexpr auto kByteArrayTypeName = BuiltinTypeName(LanguageTypeKind::ByteArray);
inline constexpr auto kByteDataTypeName = BuiltinTypeName(LanguageTypeKind::ByteData);
inline constexpr auto kRegExpTypeName = BuiltinTypeName(LanguageTypeKind::RegExp);
inline constexpr auto kRegExpMatchTypeName = BuiltinTypeName(LanguageTypeKind::RegExpMatch);
inline constexpr std::string_view kNullableRegExpMatchTypeName = "RegExpMatch?";
inline constexpr std::string_view kEndianTypeName = "Endian";
inline constexpr auto kFutureTypeName = BuiltinTypeName(LanguageTypeKind::Future);
inline constexpr auto kCompleterTypeName = BuiltinTypeName(LanguageTypeKind::Completer);
inline constexpr auto kTimerTypeName = BuiltinTypeName(LanguageTypeKind::Timer);
inline constexpr auto kCancellationTokenTypeName =
    BuiltinTypeName(LanguageTypeKind::CancellationToken);
inline constexpr auto kSetTypeName = BuiltinTypeName(LanguageTypeKind::Set);
inline constexpr auto kMapTypeName = BuiltinTypeName(LanguageTypeKind::Map);
inline constexpr std::string_view kErrorTypeName = "Error";
inline constexpr std::string_view kTimeoutExceptionTypeName = "TimeoutException";
inline constexpr std::string_view kDateTypeName = "Date";
inline constexpr std::string_view kDatePartsTypeName = "DateParts";
inline constexpr std::string_view kJsonTypeName = "JSON";
inline constexpr std::string_view kJsonParseResultTypeName = "JSONParseResult";
inline constexpr std::string_view kJsonParseErrorTypeName = "JSONParseError";
inline constexpr std::string_view kMathTypeName = "Math";
inline constexpr std::string_view kRandomTypeName = "Random";
inline constexpr std::string_view kPerformanceTypeName = "performance";
inline constexpr std::string_view kSleepFunctionName = "sleep";

inline constexpr auto kBuiltinTypeNames = std::to_array<std::string_view>({
    kNullTypeName,          kNeverTypeName,
    kVoidTypeName,          kDynamicTypeName,
    kBoolTypeName,          kIntTypeName,
    kDoubleTypeName,        kStringTypeName,
    kMutableStringTypeName, kByteArrayTypeName,
    kByteDataTypeName,      kRegExpTypeName,
    kRegExpMatchTypeName,   kEndianTypeName,
    kFutureTypeName,        kCompleterTypeName,
    kTimerTypeName,         kCancellationTokenTypeName,
    kSetTypeName,           kMapTypeName,
    kErrorTypeName,         kTimeoutExceptionTypeName,
    kDateTypeName,          kDatePartsTypeName,
    kJsonTypeName,          kMathTypeName,
    kRandomTypeName,        kPerformanceTypeName,
});

}  // namespace vela::frontend
