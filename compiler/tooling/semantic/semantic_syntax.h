#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/source_location.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/token.h"

namespace vela::frontend {
struct SourceSnapshot;
}

namespace vela::tooling {

inline constexpr std::string_view kCompletionMarker = "__vela_completion__";

template <class Declaration>
[[nodiscard]] std::span<const frontend::Attribute> DeclarationAttributes(
    const Declaration& declaration) noexcept {
  if constexpr (requires { declaration.attributes; }) {
    return declaration.attributes;
  }
  return {};
}

template <class Declaration>
[[nodiscard]] SourceLocation DocumentationLocation(const Declaration& declaration) noexcept {
  const auto attributes = DeclarationAttributes(declaration);
  return attributes.empty() ? declaration.location : attributes.front().location;
}

[[nodiscard]] inline bool IsNameLikeToken(frontend::TokenKind kind) noexcept {
  return frontend::IsMemberName(kind) || kind == frontend::TokenKind::Print;
}

[[nodiscard]] bool IsInComment(std::span<const frontend::SourceComment> comments,
                               std::size_t offset);
[[nodiscard]] bool IsCompletionExcluded(const frontend::SourceSnapshot& snapshot,
                                        std::size_t offset);
[[nodiscard]] std::optional<frontend::TokenKind> ClosingDelimiter(frontend::TokenKind kind);
void TrackDelimiter(std::vector<frontend::TokenKind>& closing,
                    frontend::TokenKind kind,
                    std::optional<frontend::TokenKind> expected);
enum class CompletionBrace : std::uint8_t { Close, TerminateStatement };
void AppendClosingDelimiters(std::string& text,
                             std::span<const frontend::TokenKind> closing,
                             CompletionBrace brace);
[[nodiscard]] std::optional<std::size_t> TokenIndexAt(std::span<const frontend::Token> tokens,
                                                      std::size_t offset);
[[nodiscard]] std::size_t TokenIndexAt(std::span<const frontend::Token> tokens,
                                       SourceLocation location);

}  // namespace vela::tooling
