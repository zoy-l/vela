#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

#include <vela/error.h>
#include <vela/handles.h>
#include <vela/types.h>

#include "runtime/native/handle_access.h"
#include "runtime/native/host_value.h"

namespace vela {

[[nodiscard]] Result<Local<String>> CreateString(HandleScope& scope, std::string_view text);
template <class T>
std::string_view Local<T>::Utf8() const noexcept
  requires std::is_same_v<T, String>
{
  return internal::HostStringUtf8(internal::HandleAccess::Read(*this));
}

[[nodiscard]] std::string CopyString(Local<String> text);
[[nodiscard]] std::int64_t StringLength(Local<String> text) noexcept;
[[nodiscard]] std::int64_t StringByteLength(Local<String> text) noexcept;
[[nodiscard]] Result<HostValue<String>> StringSlice(HandleScope& scope,
                                                    HostValue<String> text,
                                                    HostValue<std::int64_t> start,
                                                    HostValue<std::int64_t> end);
[[nodiscard]] Result<HostValue<String>> StringAt(HandleScope& scope,
                                                 HostValue<String> text,
                                                 HostValue<std::int64_t> index);
[[nodiscard]] Result<HostValue<std::int64_t>> StringIndexOf(HandleScope& scope,
                                                            HostValue<String> text,
                                                            HostValue<String> part);
[[nodiscard]] Result<HostValue<bool>> StringIncludes(HandleScope& scope,
                                                     HostValue<String> text,
                                                     HostValue<String> part);
[[nodiscard]] Result<HostValue<bool>> StringStartsWith(HandleScope& scope,
                                                       HostValue<String> text,
                                                       HostValue<String> prefix);
[[nodiscard]] Result<HostValue<bool>> StringEndsWith(HandleScope& scope,
                                                     HostValue<String> text,
                                                     HostValue<String> suffix);
[[nodiscard]] Result<HostValue<String>> StringToLowerCase(HandleScope& scope,
                                                          HostValue<String> text);
[[nodiscard]] Result<HostValue<String>> StringToUpperCase(HandleScope& scope,
                                                          HostValue<String> text);
[[nodiscard]] Result<HostValue<Array<std::int64_t>>> StringCodePoints(HandleScope& scope,
                                                                      HostValue<String> text);
[[nodiscard]] Result<HostValue<Array<String>>> StringGraphemes(HandleScope& scope,
                                                               HostValue<String> text);
[[nodiscard]] Result<HostValue<std::int64_t>> StringGraphemeLength(HandleScope& scope,
                                                                   HostValue<String> text);
[[nodiscard]] Result<HostValue<String>> StringGraphemeSlice(HandleScope& scope,
                                                            HostValue<String> text,
                                                            HostValue<std::int64_t> start,
                                                            HostValue<std::int64_t> end);
[[nodiscard]] Result<HostValue<MutableString>> CreateMutableString(HandleScope& scope);
[[nodiscard]] Result<HostValue<void>> AppendString(HandleScope& scope,
                                                   HostValue<MutableString> builder,
                                                   HostValue<String> text);
[[nodiscard]] Result<HostValue<String>> BuildString(HandleScope& scope,
                                                    HostValue<MutableString> builder);

}  // namespace vela
