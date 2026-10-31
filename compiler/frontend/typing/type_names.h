#pragma once

#include <string_view>

namespace vela::frontend {

inline constexpr std::string_view kNullTypeName = "null";
inline constexpr std::string_view kVoidTypeName = "void";
inline constexpr std::string_view kDynamicTypeName = "dynamic";
inline constexpr std::string_view kBoolTypeName = "bool";
inline constexpr std::string_view kIntTypeName = "int";
inline constexpr std::string_view kDoubleTypeName = "double";
inline constexpr std::string_view kStringTypeName = "string";
inline constexpr std::string_view kIntArrayTypeName = "int[]";
inline constexpr std::string_view kStringArrayTypeName = "string[]";
inline constexpr std::string_view kNullableStringTypeName = "string?";
inline constexpr std::string_view kMutableStringTypeName = "String";
inline constexpr std::string_view kByteArrayTypeName = "ByteArray";
inline constexpr std::string_view kByteDataTypeName = "ByteData";
inline constexpr std::string_view kRegExpTypeName = "RegExp";
inline constexpr std::string_view kRegExpMatchTypeName = "RegExpMatch";
inline constexpr std::string_view kNullableRegExpMatchTypeName = "RegExpMatch?";
inline constexpr std::string_view kEndianTypeName = "Endian";
inline constexpr std::string_view kFutureTypeName = "Future";
inline constexpr std::string_view kCompleterTypeName = "Completer";
inline constexpr std::string_view kTimerTypeName = "Timer";
inline constexpr std::string_view kCancellationTokenTypeName = "CancellationToken";
inline constexpr std::string_view kSetTypeName = "Set";
inline constexpr std::string_view kMapTypeName = "Map";
inline constexpr std::string_view kErrorTypeName = "Error";
inline constexpr std::string_view kTimeoutExceptionTypeName = "TimeoutException";
inline constexpr std::string_view kDateTypeName = "Date";
inline constexpr std::string_view kDatePartsTypeName = "DateParts";
inline constexpr std::string_view kJsonTypeName = "Json";
inline constexpr std::string_view kMathTypeName = "Math";
inline constexpr std::string_view kRandomTypeName = "Random";
inline constexpr std::string_view kPerformanceTypeName = "performance";
inline constexpr std::string_view kSleepFunctionName = "sleep";

}  // namespace vela::frontend
