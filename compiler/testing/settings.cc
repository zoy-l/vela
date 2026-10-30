#include "compiler/testing/settings.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace vela::testing {
namespace {

constexpr double kMillisecondsPerSecond = 1000.0;
constexpr double kMillisecondsPerMinute = 60000.0;
constexpr double kMillisecondsPerHour = 3600000.0;
constexpr double kMillisecondsPerDay = 86400000.0;

bool IsSpace(char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

void TrimStart(std::string_view& text) {
  while (!text.empty() && IsSpace(text.front())) {
    text.remove_prefix(1);
  }
}

std::expected<std::chrono::milliseconds, std::string> Duration(double milliseconds) {
  const auto schedulable = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::time_point::max() - std::chrono::steady_clock::now());
  if (!std::isfinite(milliseconds) || milliseconds < 0.0 ||
      milliseconds >= static_cast<double>(schedulable.count())) {
    return std::unexpected("timeout duration is not representable");
  }
  return std::chrono::milliseconds(static_cast<std::int64_t>(milliseconds));
}

std::expected<double, std::string> ReadNumber(std::string_view& text) {
  double number = 0.0;
  const auto characters = std::span(text);
  const auto* const begin = characters.data();
  const auto parsed = std::from_chars(begin, characters.last(0).data(), number);
  if (parsed.ec != std::errc() || parsed.ptr == begin || !std::isfinite(number) || number < 0.0) {
    return std::unexpected("expected a nonnegative finite timeout number");
  }
  text.remove_prefix(static_cast<std::size_t>(parsed.ptr - begin));
  return number;
}

bool IsTagCharacter(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9') || value == '_' || value == '-';
}

}  // namespace

std::expected<std::optional<std::chrono::milliseconds>, std::string> Timeout::Apply(
    std::optional<std::chrono::milliseconds> inherited) const {
  if (kind == TimeoutKind::None) {
    return std::nullopt;
  }
  if (kind == TimeoutKind::Duration) {
    const auto checked = Duration(static_cast<double>(duration.count()));
    if (!checked) {
      return std::unexpected(checked.error());
    }
    return duration;
  }
  if (!inherited) {
    return std::nullopt;
  }
  const auto scaled = Duration(static_cast<double>(inherited->count()) * factor);
  if (!scaled) {
    return std::unexpected(scaled.error());
  }
  return *scaled;
}

std::expected<Timeout, std::string> ParseTimeout(std::string_view text) {
  TrimStart(text);
  while (!text.empty() && IsSpace(text.back())) {
    text.remove_suffix(1);
  }
  if (text == "none") {
    return Timeout{.kind = TimeoutKind::None};
  }
  if (text.empty()) {
    return std::unexpected("timeout must not be empty");
  }
  double milliseconds = 0.0;
  std::size_t components = 0;
  while (!text.empty()) {
    const auto number = ReadNumber(text);
    if (!number) {
      return std::unexpected(number.error());
    }
    if (text == "x" && components == 0 && *number > 0.0) {
      const auto normalized = Duration(static_cast<double>(kDefaultTimeout.count()) * *number);
      if (!normalized) {
        return std::unexpected(normalized.error());
      }
      return Timeout{.kind = TimeoutKind::Factor, .factor = *number};
    }
    double multiplier = 0.0;
    if (text.starts_with("ms")) {
      multiplier = 1.0;
      text.remove_prefix(2);
    } else if (!text.empty()) {
      switch (text.front()) {
        case 's':
          multiplier = kMillisecondsPerSecond;
          break;
        case 'm':
          multiplier = kMillisecondsPerMinute;
          break;
        case 'h':
          multiplier = kMillisecondsPerHour;
          break;
        case 'd':
          multiplier = kMillisecondsPerDay;
          break;
        default:
          return std::unexpected("invalid timeout unit");
      }
      text.remove_prefix(1);
    } else {
      return std::unexpected("timeout requires a duration unit");
    }
    milliseconds += *number * multiplier;
    ++components;
    if (!text.empty() && !IsSpace(text.front())) {
      return std::unexpected("timeout components must be separated by whitespace");
    }
    TrimStart(text);
  }
  const auto duration = Duration(milliseconds);
  if (!duration) {
    return std::unexpected(duration.error());
  }
  return Timeout{.duration = *duration};
}

class TagSelector::Parser final {
 public:
  explicit Parser(std::string_view text) : text_(text) {}
  std::expected<TagSelector, std::string> Run();

 private:
  static int Precedence(char operation);
  void Emit();
  std::expected<void, std::string> ReadOperand();
  std::expected<void, std::string> ReadOperator();
  std::string_view text_;
  TagSelector result_;
  std::vector<char> operators_;
  bool needs_operand_ = true;
};

int TagSelector::Parser::Precedence(char operation) {
  switch (operation) {
    case '!':
      return 3;
    case '&':
      return 2;
    case '|':
      return 1;
    default:
      return 0;
  }
}

void TagSelector::Parser::Emit() {
  Operation operation = Operation::Or;
  switch (operators_.back()) {
    case '!':
      operation = Operation::Not;
      break;
    case '&':
      operation = Operation::And;
      break;
    default:
      break;
  }
  operators_.pop_back();
  result_.terms_.push_back({.operation = operation, .tag = {}});
}

std::expected<void, std::string> TagSelector::Parser::ReadOperand() {
  const char token = text_.front();
  if (token == '!' || token == '(') {
    operators_.push_back(token);
    text_.remove_prefix(1);
    return {};
  }
  std::size_t length = 0;
  while (length < text_.size() && IsTagCharacter(text_.at(length))) {
    ++length;
  }
  if (length == 0) {
    return std::unexpected("expected a tag name, !, or (");
  }
  result_.terms_.push_back(
      {.operation = Operation::Tag, .tag = std::string(text_.substr(0, length))});
  text_.remove_prefix(length);
  needs_operand_ = false;
  return {};
}

std::expected<void, std::string> TagSelector::Parser::ReadOperator() {
  const char token = text_.front();
  if (token == ')') {
    while (!operators_.empty() && operators_.back() != '(') {
      Emit();
    }
    if (operators_.empty()) {
      return std::unexpected("unmatched ) in tag selector");
    }
    operators_.pop_back();
    text_.remove_prefix(1);
    return {};
  }
  if (!text_.starts_with("&&") && !text_.starts_with("||")) {
    return std::unexpected("expected &&, ||, or ) in tag selector");
  }
  while (!operators_.empty() && Precedence(operators_.back()) >= Precedence(token)) {
    Emit();
  }
  operators_.push_back(token);
  text_.remove_prefix(2);
  needs_operand_ = true;
  return {};
}

std::expected<TagSelector, std::string> TagSelector::Parser::Run() {
  TrimStart(text_);
  while (!text_.empty()) {
    const auto read = needs_operand_ ? ReadOperand() : ReadOperator();
    if (!read) {
      return std::unexpected(read.error());
    }
    TrimStart(text_);
  }
  if (needs_operand_) {
    return std::unexpected("tag selector requires an operand");
  }
  while (!operators_.empty()) {
    if (operators_.back() == '(') {
      return std::unexpected("unclosed ( in tag selector");
    }
    Emit();
  }
  return std::move(result_);
}

std::expected<TagSelector, std::string> TagSelector::Parse(std::string_view text) {
  return Parser(text).Run();
}

bool TagSelector::Matches(std::span<const std::string> tags) const {
  std::vector<std::uint8_t> values;
  for (const auto& term : terms_) {
    if (term.operation == Operation::Tag) {
      values.push_back(static_cast<std::uint8_t>(std::ranges::find(tags, term.tag) != tags.end()));
    } else if (term.operation == Operation::Not) {
      values.back() = static_cast<std::uint8_t>(values.back() == 0);
    } else {
      const bool right = values.back() != 0;
      values.pop_back();
      const bool left = values.back() != 0;
      values.back() = static_cast<std::uint8_t>(term.operation == Operation::And ? left && right
                                                                                 : left || right);
    }
  }
  return !values.empty() && values.back() != 0;
}

std::expected<Reporter, std::string> ParseReporter(std::string_view text) {
  if (text == "compact") {
    return Reporter::Compact;
  }
  if (text == "expanded") {
    return Reporter::Expanded;
  }
  if (text == "json") {
    return Reporter::Json;
  }
  return std::unexpected("reporter must be compact, expanded, or json");
}

std::vector<std::string_view> TagSelector::Tags() const {
  std::vector<std::string_view> tags;
  for (const auto& term : terms_) {
    if (term.operation == Operation::Tag) {
      tags.push_back(term.tag);
    }
  }
  return tags;
}

Settings::Settings() : concurrency(std::max(1U, std::thread::hardware_concurrency() / 2U)) {
#ifndef _WIN32
  reporter = Reporter::Compact;
#endif
}

}  // namespace vela::testing
