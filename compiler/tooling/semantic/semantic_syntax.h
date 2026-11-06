#pragma once

#include <cstddef>
#include <optional>
#include <span>
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

}  // namespace vela::tooling
