#include <vela/regexp.h>

#include <cstdint>

#include <vela/error.h>
#include <vela/handles.h>
#include <vela/types.h>

#include "base/builtin.h"
#include "base/regexp.h"
#include "runtime/native/host_builtin.h"

namespace vela {

Result<Local<RegExp>> CreateRegExp(HandleScope& scope,
                                   Local<String> pattern,
                                   RegExpOptions options) {
  return internal::CallHostBuiltin<RegExp, String, bool, bool, bool>(
      scope, Builtin::NewRegExp, pattern, options.multi_line, options.case_sensitive,
      options.dot_all);
}

Result<HostValue<String>> EscapeRegExp(HandleScope& scope, HostValue<String> text) {
  return internal::CallHostBuiltin<String, String>(scope, Builtin::RegExpEscape, text);
}

Result<HostValue<String>> RegExpPattern(HandleScope& scope, HostValue<RegExp> pattern) {
  return internal::CallHostBuiltin<String, RegExp>(scope, Builtin::RegExpPattern, pattern);
}

Result<HostValue<bool>> RegExpIsMultiLine(HandleScope& scope, HostValue<RegExp> pattern) {
  return internal::CallHostBuiltin<bool, RegExp>(scope, Builtin::RegExpIsMultiLine, pattern);
}

Result<HostValue<bool>> RegExpIsCaseSensitive(HandleScope& scope, HostValue<RegExp> pattern) {
  return internal::CallHostBuiltin<bool, RegExp>(scope, Builtin::RegExpIsCaseSensitive, pattern);
}

Result<HostValue<bool>> RegExpIsDotAll(HandleScope& scope, HostValue<RegExp> pattern) {
  return internal::CallHostBuiltin<bool, RegExp>(scope, Builtin::RegExpIsDotAll, pattern);
}

Result<HostValue<bool>> RegExpIsUnicode(HandleScope& scope, HostValue<RegExp> pattern) {
  return internal::CallHostBuiltin<bool, RegExp>(scope, Builtin::RegExpIsUnicode, pattern);
}

Result<HostValue<bool>> RegExpHasMatch(HandleScope& scope,
                                       HostValue<RegExp> pattern,
                                       HostValue<String> input) {
  return internal::CallHostBuiltin<bool, RegExp, String>(scope, Builtin::RegExpHasMatch, pattern,
                                                         input);
}

Result<HostValue<Nullable<RegExpMatch>>> RegExpFirstMatch(HandleScope& scope,
                                                          HostValue<RegExp> pattern,
                                                          HostValue<String> input) {
  return internal::CallHostBuiltin<Nullable<RegExpMatch>, RegExp, String>(
      scope, Builtin::RegExpFirstMatch, pattern, input);
}

Result<HostValue<Array<RegExpMatch>>> RegExpAllMatches(HandleScope& scope,
                                                       HostValue<RegExp> pattern,
                                                       HostValue<String> input,
                                                       HostValue<std::int64_t> start) {
  return internal::CallHostBuiltin<Array<RegExpMatch>, RegExp, String, std::int64_t>(
      scope, Builtin::RegExpAllMatches, pattern, input, start);
}

Result<HostValue<Nullable<RegExpMatch>>> RegExpMatchAsPrefix(HandleScope& scope,
                                                             HostValue<RegExp> pattern,
                                                             HostValue<String> input,
                                                             HostValue<std::int64_t> start) {
  return internal::CallHostBuiltin<Nullable<RegExpMatch>, RegExp, String, std::int64_t>(
      scope, Builtin::RegExpMatchAsPrefix, pattern, input, start);
}

Result<HostValue<Nullable<String>>> RegExpStringMatch(HandleScope& scope,
                                                      HostValue<RegExp> pattern,
                                                      HostValue<String> input) {
  return internal::CallHostBuiltin<Nullable<String>, RegExp, String>(
      scope, Builtin::RegExpStringMatch, pattern, input);
}

Result<HostValue<std::int64_t>> RegExpMatchStart(HandleScope& scope, HostValue<RegExpMatch> match) {
  return internal::CallHostBuiltin<std::int64_t, RegExpMatch>(scope, Builtin::RegExpMatchStart,
                                                              match);
}

Result<HostValue<std::int64_t>> RegExpMatchEnd(HandleScope& scope, HostValue<RegExpMatch> match) {
  return internal::CallHostBuiltin<std::int64_t, RegExpMatch>(scope, Builtin::RegExpMatchEnd,
                                                              match);
}

Result<HostValue<String>> RegExpMatchInput(HandleScope& scope, HostValue<RegExpMatch> match) {
  return internal::CallHostBuiltin<String, RegExpMatch>(scope, Builtin::RegExpMatchInput, match);
}

Result<HostValue<RegExp>> RegExpMatchPattern(HandleScope& scope, HostValue<RegExpMatch> match) {
  return internal::CallHostBuiltin<RegExp, RegExpMatch>(scope, Builtin::RegExpMatchPattern, match);
}

Result<HostValue<std::int64_t>> RegExpMatchGroupCount(HandleScope& scope,
                                                      HostValue<RegExpMatch> match) {
  return internal::CallHostBuiltin<std::int64_t, RegExpMatch>(scope, Builtin::RegExpMatchGroupCount,
                                                              match);
}

Result<HostValue<Array<String>>> RegExpMatchGroupNames(HandleScope& scope,
                                                       HostValue<RegExpMatch> match) {
  return internal::CallHostBuiltin<Array<String>, RegExpMatch>(
      scope, Builtin::RegExpMatchGroupNames, match);
}

Result<HostValue<Nullable<String>>> RegExpMatchGroup(HandleScope& scope,
                                                     HostValue<RegExpMatch> match,
                                                     HostValue<std::int64_t> index) {
  return internal::CallHostBuiltin<Nullable<String>, RegExpMatch, std::int64_t>(
      scope, Builtin::RegExpMatchGroup, match, index);
}

Result<HostValue<Array<Nullable<String>>>> RegExpMatchGroups(
    HandleScope& scope,
    HostValue<RegExpMatch> match,
    HostValue<Array<std::int64_t>> indices) {
  return internal::CallHostBuiltin<Array<Nullable<String>>, RegExpMatch, Array<std::int64_t>>(
      scope, Builtin::RegExpMatchGroups, match, indices);
}

Result<HostValue<Nullable<String>>> RegExpMatchNamedGroup(HandleScope& scope,
                                                          HostValue<RegExpMatch> match,
                                                          HostValue<String> name) {
  return internal::CallHostBuiltin<Nullable<String>, RegExpMatch, String>(
      scope, Builtin::RegExpMatchNamedGroup, match, name);
}

}  // namespace vela
