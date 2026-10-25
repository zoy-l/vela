#include "compiler/tooling/file_uri.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "base/path.h"
#include "base/utf8.h"

namespace vela::tooling {
namespace {

constexpr int kHexLetterOffset = 10;

auto AsciiLower(char character) -> char {
  return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                              : character;
}

auto IsSameAscii(std::string_view left, std::string_view right) -> bool {
  return std::ranges::equal(left, right, {}, AsciiLower, AsciiLower);
}

auto HexDigit(char character) -> int {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + kHexLetterOffset;
  }
  if (character >= 'A' && character <= 'F') {
    return character - 'A' + kHexLetterOffset;
  }
  return -1;
}

auto Decode(std::string_view text) -> std::expected<std::string, std::string> {
  std::string decoded;
  decoded.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    const auto character = text.at(index);
    if (character != '%') {
      decoded.push_back(character);
      continue;
    }
    if (text.size() - index < 3) {
      return std::unexpected("truncated URI escape");
    }
    const auto high = HexDigit(text.at(index + 1));
    const auto low = HexDigit(text.at(index + 2));
    if (high < 0 || low < 0) {
      return std::unexpected("invalid URI escape");
    }
    constexpr int kHexBase = 16;
    decoded.push_back(static_cast<char>((high * kHexBase) + low));
    index += 2;
  }
  if (decoded.contains('\0') || InvalidUtf8Offset(decoded)) {
    return std::unexpected("file URI is not a valid UTF-8 path");
  }
  return decoded;
}

auto IsUriPathCharacter(unsigned char character) -> bool {
  return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9') || character == '-' || character == '_' ||
         character == '.' || character == '~' || character == '/' || character == ':';
}

}  // namespace

auto FileUriPath(std::string_view uri) -> std::expected<std::filesystem::path, std::string> {
  constexpr std::string_view kScheme = "file://";
  if (!IsSameAscii(uri.substr(0, kScheme.size()), kScheme)) {
    return std::unexpected("only absolute file:// document URIs are supported");
  }
  const auto suffix = uri.substr(kScheme.size());
  if (suffix.contains('?') || suffix.contains('#')) {
    return std::unexpected("file URI cannot contain a query or fragment");
  }
  const auto slash = suffix.find('/');
  if (slash == std::string_view::npos) {
    return std::unexpected("file URI requires an absolute path");
  }
  const auto authority = Decode(suffix.substr(0, slash));
  if (!authority) {
    return std::unexpected(authority.error());
  }
  auto decoded = Decode(suffix.substr(slash));
  if (!decoded) {
    return std::unexpected(decoded.error());
  }
  auto text = std::move(*decoded);
  const auto is_local = authority->empty() || IsSameAscii(*authority, "localhost");
  if (!is_local) {
    if (authority->contains('/') || authority->contains('@') || authority->contains(':') ||
        authority->contains('\\')) {
      return std::unexpected("invalid file URI authority");
    }
    text = "//" + *authority + text;
  }
#ifdef _WIN32
  if (is_local) {
    if (text.size() < 4 || text.at(2) != ':' || text.at(3) != '/') {
      return std::unexpected("Windows file URI requires an absolute drive path");
    }
    text.erase(0, 1);
  }
#endif
  auto path = Utf8Path(text);
  if (!path.is_absolute()) {
    return std::unexpected("file URI path is not absolute");
  }
  return path.lexically_normal();
}

auto FileUri(const std::filesystem::path& path) -> std::string {
  const auto text = PathText(path);
  std::string uri = "file:///";
  if (text.starts_with("//")) {
    uri = "file:";
  } else if (text.starts_with('/')) {
    uri = "file://";
  }
  constexpr std::string_view kHex = "0123456789ABCDEF";
  for (const auto character : text) {
    const auto byte = static_cast<unsigned char>(character);
    if (IsUriPathCharacter(byte)) {
      uri.push_back(character);
    } else {
      constexpr unsigned kNibbleBits = 4;
      constexpr unsigned kNibbleMask = 0xf;
      uri.push_back('%');
      uri.push_back(kHex.at(byte >> kNibbleBits));
      uri.push_back(kHex.at(byte & kNibbleMask));
    }
  }
  return uri;
}

}  // namespace vela::tooling
