#include <vela/strings.h>

#include <cstdint>
#include <string>
#include <string_view>

#include <vela/error.h>
#include <vela/handles.h>
#include <vela/types.h>

#include "base/builtin.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/host_builtin.h"
#include "runtime/native/host_value.h"
#include "runtime/strings/string.h"
#include "runtime/value/value.h"

namespace vela {

Result<Local<String>> CreateString(HandleScope& scope, std::string_view text) {
  return internal::ReceiveHostValue<String>(scope, [&](runtime::Value& output) {
    return internal::CreateHostString(scope, text, output);
  });
}

std::string CopyString(Local<String> text) {
  return std::string(internal::HandleAccess::Read(text).AsString()->value());
}

std::int64_t StringLength(Local<String> text) noexcept {
  return static_cast<std::int64_t>(internal::HandleAccess::Read(text).AsString()->length());
}

std::int64_t StringByteLength(Local<String> text) noexcept {
  return static_cast<std::int64_t>(internal::HandleAccess::Read(text).AsString()->value().size());
}

Result<HostValue<String>> StringSlice(HandleScope& scope,
                                      HostValue<String> text,
                                      HostValue<std::int64_t> start,
                                      HostValue<std::int64_t> end) {
  return internal::CallHostBuiltin<String, String, std::int64_t, std::int64_t>(
      scope, Builtin::StringSlice, text, start, end);
}

Result<HostValue<String>> StringAt(HandleScope& scope,
                                   HostValue<String> text,
                                   HostValue<std::int64_t> index) {
  return internal::CallHostBuiltin<String, String, std::int64_t>(scope, Builtin::StringIndex, text,
                                                                 index);
}

Result<HostValue<std::int64_t>> StringIndexOf(HandleScope& scope,
                                              HostValue<String> text,
                                              HostValue<String> part) {
  return internal::CallHostBuiltin<std::int64_t, String, String>(scope, Builtin::StringIndexOf,
                                                                 text, part);
}

Result<HostValue<bool>> StringIncludes(HandleScope& scope,
                                       HostValue<String> text,
                                       HostValue<String> part) {
  return internal::CallHostBuiltin<bool, String, String>(scope, Builtin::StringIncludes, text,
                                                         part);
}

Result<HostValue<bool>> StringStartsWith(HandleScope& scope,
                                         HostValue<String> text,
                                         HostValue<String> prefix) {
  return internal::CallHostBuiltin<bool, String, String>(scope, Builtin::StringStartsWith, text,
                                                         prefix);
}

Result<HostValue<bool>> StringEndsWith(HandleScope& scope,
                                       HostValue<String> text,
                                       HostValue<String> suffix) {
  return internal::CallHostBuiltin<bool, String, String>(scope, Builtin::StringEndsWith, text,
                                                         suffix);
}

Result<HostValue<String>> StringToLowerCase(HandleScope& scope, HostValue<String> text) {
  return internal::CallHostBuiltin<String, String>(scope, Builtin::StringToLowerCase, text);
}

Result<HostValue<String>> StringToUpperCase(HandleScope& scope, HostValue<String> text) {
  return internal::CallHostBuiltin<String, String>(scope, Builtin::StringToUpperCase, text);
}

Result<HostValue<Array<std::int64_t>>> StringCodePoints(HandleScope& scope,
                                                        HostValue<String> text) {
  return internal::CallHostBuiltin<Array<std::int64_t>, String>(scope, Builtin::StringCodePoints,
                                                                text);
}

Result<HostValue<Array<String>>> StringGraphemes(HandleScope& scope, HostValue<String> text) {
  return internal::CallHostBuiltin<Array<String>, String>(scope, Builtin::StringGraphemes, text);
}

Result<HostValue<std::int64_t>> StringGraphemeLength(HandleScope& scope, HostValue<String> text) {
  return internal::CallHostBuiltin<std::int64_t, String>(scope, Builtin::StringGraphemeLength,
                                                         text);
}

Result<HostValue<String>> StringGraphemeSlice(HandleScope& scope,
                                              HostValue<String> text,
                                              HostValue<std::int64_t> start,
                                              HostValue<std::int64_t> end) {
  return internal::CallHostBuiltin<String, String, std::int64_t, std::int64_t>(
      scope, Builtin::StringGraphemeSlice, text, start, end);
}

Result<HostValue<MutableString>> CreateMutableString(HandleScope& scope) {
  return internal::CallHostBuiltin<MutableString>(scope, Builtin::NewMutableString);
}

Result<HostValue<void>> AppendString(HandleScope& scope,
                                     HostValue<MutableString> builder,
                                     HostValue<String> text) {
  return internal::CallHostBuiltin<void, MutableString, String>(scope, Builtin::MutableStringAppend,
                                                                builder, text);
}

Result<HostValue<String>> BuildString(HandleScope& scope, HostValue<MutableString> builder) {
  return internal::CallHostBuiltin<String, MutableString>(scope, Builtin::MutableStringToString,
                                                          builder);
}

}  // namespace vela
