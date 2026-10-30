#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vela::testing {

inline constexpr std::chrono::milliseconds kDefaultTimeout{30000};

enum class TimeoutKind : std::uint8_t { Duration, Factor, None };

struct Timeout final {
  TimeoutKind kind = TimeoutKind::Duration;
  std::chrono::milliseconds duration = kDefaultTimeout;
  double factor = 1.0;

  [[nodiscard]] std::expected<std::optional<std::chrono::milliseconds>, std::string> Apply(
      std::optional<std::chrono::milliseconds> inherited) const;
};

[[nodiscard]] std::expected<Timeout, std::string> ParseTimeout(std::string_view text);

// 后缀表达式在配置边界构造；求值不解析文本，也不依赖标签容器的顺序。
class TagSelector final {
 public:
  [[nodiscard]] static std::expected<TagSelector, std::string> Parse(std::string_view text);
  [[nodiscard]] bool Matches(std::span<const std::string> tags) const;
  [[nodiscard]] std::vector<std::string_view> Tags() const;

 private:
  class Parser;
  enum class Operation : std::uint8_t { Tag, Not, And, Or };
  struct Term final {
    Operation operation;
    std::string tag;
  };
  std::vector<Term> terms_;
};

enum class Reporter : std::uint8_t { Compact, Expanded, Json };
[[nodiscard]] std::expected<Reporter, std::string> ParseReporter(std::string_view text);

struct Skip final {
  bool enabled = false;
  std::string reason;
};

struct TagConfiguration final {
  std::string selector_text;
  TagSelector selector;
  std::optional<Timeout> timeout;
  std::optional<Skip> skip;
};

struct Settings final {
  Settings();
  std::vector<std::string> paths;
  std::string filename = "*_test.vela";
  Timeout timeout;
  Timeout suite_load_timeout{.kind = TimeoutKind::None};
  std::uint32_t concurrency = 1;
  Reporter reporter = Reporter::Expanded;
  std::optional<TagSelector> include_tags;
  std::optional<TagSelector> exclude_tags;
  std::vector<TagConfiguration> tags;
};

}  // namespace vela::testing
