#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "compiler/testing/pattern.h"
#include "compiler/testing/settings.h"

namespace vela::testing {

struct TestCommandOptions final {
  std::vector<std::string> paths;
  std::optional<std::filesystem::path> config;
  std::optional<Timeout> timeout;
  std::optional<std::uint32_t> concurrency;
  std::optional<Reporter> reporter;
  std::optional<TagSelector> include_tags;
  std::optional<TagSelector> exclude_tags;
  std::vector<Pattern> names;
  std::vector<std::string> plain_names;
  bool list = false;
  bool help = false;

  void Apply(Settings& settings) const;
  [[nodiscard]] std::expected<bool, std::string> IsSelected(std::string_view name) const;
};

[[nodiscard]] std::expected<TestCommandOptions, std::string> ParseTestCommandOptions(
    std::span<const std::string_view> arguments);

}  // namespace vela::testing
