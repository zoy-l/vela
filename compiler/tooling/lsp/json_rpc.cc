#include "compiler/tooling/lsp/json_rpc.h"

#include <charconv>
#include <cstddef>
#include <expected>
#include <ios>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "base/json.h"

namespace vela::tooling {
namespace {

constexpr std::size_t kMaximumHeaderBytes = 8192;

std::expected<std::string, RpcError> HeaderLine(std::istream& input) {
  std::string line;
  char character = 0;
  while (input.get(character)) {
    if (character == '\n') {
      if (!line.ends_with('\r')) {
        return std::unexpected(RpcError{.message = "RPC header requires CRLF"});
      }
      line.pop_back();
      return line;
    }
    line.push_back(character);
    if (line.size() > kMaximumHeaderBytes) {
      return std::unexpected(RpcError{.message = "RPC header is too large"});
    }
  }
  return std::unexpected(RpcError{.message = "truncated RPC header"});
}

std::expected<std::size_t, RpcError> ReadLength(std::istream& input) {
  std::optional<std::size_t> length;
  std::size_t header_bytes = 0;
  while (true) {
    const auto line = HeaderLine(input);
    if (!line) {
      return std::unexpected(line.error());
    }
    header_bytes += line->size() + 2;
    if (header_bytes > kMaximumHeaderBytes) {
      return std::unexpected(RpcError{.message = "RPC header is too large"});
    }
    if (line->empty()) {
      break;
    }
    const auto colon = line->find(':');
    if (colon == std::string::npos) {
      return std::unexpected(RpcError{.message = "invalid RPC header field"});
    }
    auto name = line->substr(0, colon);
    for (auto& character : name) {
      if (character >= 'A' && character <= 'Z') {
        character = static_cast<char>(character - 'A' + 'a');
      }
    }
    if (name != "content-length") {
      continue;
    }
    if (length) {
      return std::unexpected(RpcError{.message = "duplicate Content-Length"});
    }
    auto value = std::string_view(*line).substr(colon + 1);
    const auto start = value.find_first_not_of(" \t");
    if (start == std::string_view::npos) {
      return std::unexpected(RpcError{.message = "empty Content-Length"});
    }
    value.remove_prefix(start);
    const auto last = value.find_last_not_of(" \t");
    value = value.substr(0, last + 1);
    std::size_t bytes = 0;
    const auto* end = std::to_address(value.end());
    const auto* begin = std::to_address(value.begin());
    const auto parsed = std::from_chars(begin, end, bytes);
    if (parsed.ec != std::errc() || parsed.ptr != end || bytes > kMaximumRpcMessageBytes) {
      return std::unexpected(RpcError{.message = "invalid or oversized Content-Length"});
    }
    length = bytes;
  }
  if (!length) {
    return std::unexpected(RpcError{.message = "Content-Length is required"});
  }
  return *length;
}

}  // namespace

std::expected<std::optional<JsonDocument>, RpcError> ReadRpcMessage(std::istream& input) {
  if (input.peek() == std::char_traits<char>::eof() && input.eof()) {
    return std::nullopt;
  }
  const auto length = ReadLength(input);
  if (!length) {
    return std::unexpected(length.error());
  }
  std::string text(*length, '\0');
  if (!input.read(text.data(), static_cast<std::streamsize>(text.size()))) {
    return std::unexpected(RpcError{.message = "truncated RPC body"});
  }
  auto parsed = JsonDocument::Parse(text);
  if (!parsed) {
    return std::unexpected(RpcError{.kind = RpcErrorKind::Json, .message = parsed.error().message});
  }
  return std::optional<JsonDocument>(std::move(*parsed));
}

std::expected<void, std::string> WriteRpcMessage(std::ostream& output, std::string_view text) {
  if (text.size() > kMaximumRpcMessageBytes) {
    return std::unexpected("RPC output is too large");
  }
  output << "Content-Length: " << text.size() << "\r\n\r\n";
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  output.flush();
  if (!output) {
    return std::unexpected("cannot write RPC message");
  }
  return {};
}

}  // namespace vela::tooling
