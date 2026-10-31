#pragma once

#include <optional>
#include <vector>

#include "compiler/frontend/token.h"

namespace vela::tooling {

[[nodiscard]] bool IsIdentifierByte(char value);
[[nodiscard]] std::optional<frontend::TokenKind> ClosingDelimiter(frontend::TokenKind kind);
void TrackDelimiter(std::vector<frontend::TokenKind>& closing,
                    frontend::TokenKind kind,
                    std::optional<frontend::TokenKind> expected);

}  // namespace vela::tooling
