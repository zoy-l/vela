#pragma once

#include <cstdint>

#include <vela/error.h>
#include <vela/handles.h>
#include <vela/types.h>

#include "base/regexp.h"

namespace vela {

[[nodiscard]] Result<Local<RegExp>> CreateRegExp(HandleScope& scope,
                                                 Local<String> pattern,
                                                 RegExpOptions options = {});
[[nodiscard]] Result<HostValue<String>> EscapeRegExp(HandleScope& scope, HostValue<String> text);
[[nodiscard]] Result<HostValue<String>> RegExpPattern(HandleScope& scope,
                                                      HostValue<RegExp> pattern);
[[nodiscard]] Result<HostValue<bool>> RegExpIsMultiLine(HandleScope& scope,
                                                        HostValue<RegExp> pattern);
[[nodiscard]] Result<HostValue<bool>> RegExpIsCaseSensitive(HandleScope& scope,
                                                            HostValue<RegExp> pattern);
[[nodiscard]] Result<HostValue<bool>> RegExpIsDotAll(HandleScope& scope, HostValue<RegExp> pattern);
[[nodiscard]] Result<HostValue<bool>> RegExpIsUnicode(HandleScope& scope,
                                                      HostValue<RegExp> pattern);
[[nodiscard]] Result<HostValue<bool>> RegExpHasMatch(HandleScope& scope,
                                                     HostValue<RegExp> pattern,
                                                     HostValue<String> input);
[[nodiscard]] Result<HostValue<Nullable<RegExpMatch>>> RegExpFirstMatch(HandleScope& scope,
                                                                        HostValue<RegExp> pattern,
                                                                        HostValue<String> input);
[[nodiscard]] Result<HostValue<Array<RegExpMatch>>> RegExpAllMatches(
    HandleScope& scope,
    HostValue<RegExp> pattern,
    HostValue<String> input,
    HostValue<std::int64_t> start = 0);
[[nodiscard]] Result<HostValue<Nullable<RegExpMatch>>> RegExpMatchAsPrefix(
    HandleScope& scope,
    HostValue<RegExp> pattern,
    HostValue<String> input,
    HostValue<std::int64_t> start = 0);
[[nodiscard]] Result<HostValue<Nullable<String>>> RegExpStringMatch(HandleScope& scope,
                                                                    HostValue<RegExp> pattern,
                                                                    HostValue<String> input);
[[nodiscard]] Result<HostValue<std::int64_t>> RegExpMatchStart(HandleScope& scope,
                                                               HostValue<RegExpMatch> match);
[[nodiscard]] Result<HostValue<std::int64_t>> RegExpMatchEnd(HandleScope& scope,
                                                             HostValue<RegExpMatch> match);
[[nodiscard]] Result<HostValue<String>> RegExpMatchInput(HandleScope& scope,
                                                         HostValue<RegExpMatch> match);
[[nodiscard]] Result<HostValue<RegExp>> RegExpMatchPattern(HandleScope& scope,
                                                           HostValue<RegExpMatch> match);
[[nodiscard]] Result<HostValue<std::int64_t>> RegExpMatchGroupCount(HandleScope& scope,
                                                                    HostValue<RegExpMatch> match);
[[nodiscard]] Result<HostValue<Array<String>>> RegExpMatchGroupNames(HandleScope& scope,
                                                                     HostValue<RegExpMatch> match);
[[nodiscard]] Result<HostValue<Nullable<String>>> RegExpMatchGroup(HandleScope& scope,
                                                                   HostValue<RegExpMatch> match,
                                                                   HostValue<std::int64_t> index);
[[nodiscard]] Result<HostValue<Array<Nullable<String>>>> RegExpMatchGroups(
    HandleScope& scope,
    HostValue<RegExpMatch> match,
    HostValue<Array<std::int64_t>> indices);
[[nodiscard]] Result<HostValue<Nullable<String>>>
RegExpMatchNamedGroup(HandleScope& scope, HostValue<RegExpMatch> match, HostValue<String> name);

}  // namespace vela
