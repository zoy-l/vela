#pragma once

#include <string_view>

namespace vela::compiler::test {

inline constexpr std::string_view kRecursiveStringGcSource = R"(
func descend(depth: int): int {
  const text = '${depth}';
  if (depth == 0) { return text.length + 41; }
  const answer = descend(depth - 1);
  return text.length > 0 ? answer : 0;
}
export func main(): int { return descend(512); }
)";

inline constexpr std::string_view kUnicodeConstructionSource = R"(
export func main(): int {
  const text = 'A中😀e\u0301\0';
  const joined = [text, text].join('界');
  const absent: string? = null;
  const formatted = '${text}/${42}/${true}/${absent}';
  const numbers = [12, 34].join('中');
  const builder = String(); builder.append(text); builder.append('ß');
  const snapshot = builder.toString();
  const forward = String(); const backward = String();
  const dynamicText: dynamic = text;
  for (let i = 0; i < dynamicText.length; i++) { forward.append(dynamicText[i]); }
  for (let i = text.length - 1; i >= 0; i--) { backward.append(text[i]); }
  return text.length == 6 && joined.length == 13 && formatted.length == 19 &&
         numbers.length == 5 && snapshot.length == 7 && snapshot.toUpperCase().length == 8 &&
         'İßﬃ'.toLowerCase().length == 4 && 'ßﬃ'.toUpperCase().length == 5 &&
         forward.toString() == text && backward.toString() == '\0\u0301e😀中A' &&
         joined.slice(6, 9) == '界A中' ? 42 : 0;
}
)";

}  // namespace vela::compiler::test
