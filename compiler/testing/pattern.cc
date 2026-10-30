#include "compiler/testing/pattern.h"

#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <pcre2.h>

namespace vela::testing {
namespace {

constexpr std::size_t kErrorCapacity = 256;

std::string RegexError(int code) {
  std::array<PCRE2_UCHAR, kErrorCapacity> buffer{};
  const auto length = pcre2_get_error_message(code, buffer.data(), buffer.size());
  const void* data = buffer.data();
  return length >= 0 ? std::string(static_cast<const char*>(data), static_cast<std::size_t>(length))
                     : "regular expression error " + std::to_string(code);
}

void Literal(std::string& output, char character) {
  if (std::string_view("\\.^$|()[]{}*+?").contains(character)) {
    output.push_back('\\');
  }
  output.push_back(character);
}

class GlobTranslator final {
 public:
  explicit GlobTranslator(std::string_view source) : source_(source) {}
  std::expected<std::string, std::string> Run();

 private:
  std::expected<void, std::string> CharacterClass();
  std::string_view source_;
  std::size_t cursor_ = 0;
  std::size_t braces_ = 0;
  std::string output_ = "\\A(?:";
};

std::expected<void, std::string> GlobTranslator::CharacterClass() {
  output_.push_back('[');
  if (cursor_ < source_.size() && (source_.at(cursor_) == '!' || source_.at(cursor_) == '^')) {
    output_.push_back('^');
    ++cursor_;
  }
  const auto first = cursor_;
  while (cursor_ < source_.size() && source_.at(cursor_) != ']') {
    const char character = source_.at(cursor_++);
    if (character == '\\' || character == '[') {
      output_.push_back('\\');
    }
    output_.push_back(character);
  }
  if (cursor_ == source_.size() || cursor_ == first) {
    return std::unexpected("unclosed or empty glob character class");
  }
  ++cursor_;
  output_.push_back(']');
  return {};
}

std::expected<std::string, std::string> GlobTranslator::Run() {
  if (source_.empty()) {
    return std::unexpected("filename glob must not be empty");
  }
  while (cursor_ < source_.size()) {
    const char character = source_.at(cursor_++);
    switch (character) {
      case '*':
        output_ += "[^/]*";
        break;
      case '?':
        output_ += "[^/]";
        break;
      case '[': {
        const auto parsed = CharacterClass();
        if (!parsed) {
          return std::unexpected(parsed.error());
        }
        break;
      }
      case '{':
        ++braces_;
        output_ += "(?:";
        break;
      case '}':
        if (braces_ == 0) {
          return std::unexpected("unmatched } in filename glob");
        }
        --braces_;
        output_.push_back(')');
        break;
      case ',':
        if (braces_ == 0) {
          Literal(output_, character);
        } else {
          output_.push_back('|');
        }
        break;
      case '\\':
        if (cursor_ == source_.size()) {
          return std::unexpected("trailing glob escape");
        }
        Literal(output_, source_.at(cursor_++));
        break;
      default:
        Literal(output_, character);
        break;
    }
  }
  if (braces_ != 0) {
    return std::unexpected("unclosed { in filename glob");
  }
  output_ += ")\\z";
  return std::move(output_);
}

}  // namespace

void Pattern::Deleter::operator()(pcre2_code* code) const noexcept {
  pcre2_code_free(code);
}

std::expected<Pattern, std::string> Pattern::Regex(std::string_view source) {
  int error = 0;
  PCRE2_SIZE offset = 0;
  const void* data = source.data();
  auto* code =
      pcre2_compile(static_cast<PCRE2_SPTR>(data), source.size(),
                    PCRE2_UTF | PCRE2_UCP | PCRE2_NEVER_BACKSLASH_C, &error, &offset, nullptr);
  if (code == nullptr) {
    return std::unexpected(RegexError(error) + " at byte " + std::to_string(offset));
  }
  return Pattern(code);
}

std::expected<Pattern, std::string> Pattern::Glob(std::string_view source) {
  const auto expression = GlobTranslator(source).Run();
  if (!expression) {
    return std::unexpected(expression.error());
  }
  return Regex(*expression);
}

std::expected<bool, std::string> Pattern::IsMatch(std::string_view text) const {
  const std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)> match(
      pcre2_match_data_create_from_pattern(code_.get(), nullptr), &pcre2_match_data_free);
  if (!match) {
    return std::unexpected("cannot allocate pattern match data");
  }
  const void* data = text.data();
  const auto matched = pcre2_match(code_.get(), static_cast<PCRE2_SPTR>(data), text.size(), 0, 0,
                                   match.get(), nullptr);
  if (matched == PCRE2_ERROR_NOMATCH) {
    return false;
  }
  if (matched < 0) {
    return std::unexpected(RegexError(matched));
  }
  return true;
}

}  // namespace vela::testing
