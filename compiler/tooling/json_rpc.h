#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

#include "base/json.h"

namespace vela::tooling {

inline constexpr std::size_t kMaximumRpcMessageBytes = std::size_t{64} * 1024 * 1024;
enum class RpcErrorKind : std::uint8_t { Transport, Json };
struct RpcError final {
  RpcErrorKind kind = RpcErrorKind::Transport;
  std::string message;
};

// 正常 EOF 是 nullopt；完整帧的 JSON 错误允许下一帧继续，传输错误则应结束会话。
[[nodiscard]] auto ReadRpcMessage(std::istream& input)
    -> std::expected<std::optional<JsonDocument>, RpcError>;
[[nodiscard]] auto WriteRpcMessage(std::ostream& output, std::string_view text)
    -> std::expected<void, std::string>;

}  // namespace vela::tooling
