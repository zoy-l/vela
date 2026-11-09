#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "compiler/frontend/syntax/token.h"

namespace vela::frontend {
struct SourceSnapshot;
}

namespace vela::tooling {

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

}  // namespace vela::tooling
