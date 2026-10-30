#include "compiler/testing/command_options.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "base/path.h"
#include "compiler/testing/pattern.h"
#include "compiler/testing/settings.h"

namespace vela::testing {
namespace {

struct Option final {
  std::string_view name;
  std::string_view value;
};
std::expected<void, std::string> ReadRunnerOption(TestCommandOptions& options, Option option) {
  const auto [name, value] = option;
  if (name == "--config") {
    if (value.empty()) {
      return std::unexpected("--config requires a nonempty path");
    }
    options.config = Utf8Path(value);
  } else if (name == "--timeout") {
    const auto timeout = ParseTimeout(value);
    if (!timeout) {
      return std::unexpected(timeout.error());
    }
    options.timeout = *timeout;
  } else if (name == "--reporter") {
    const auto reporter = ParseReporter(value);
    if (!reporter) {
      return std::unexpected(reporter.error());
    }
    options.reporter = *reporter;
  } else if (name == "--concurrency") {
    std::uint32_t count = 0;
    const auto bytes = std::span(value);
    const auto parsed = std::from_chars(bytes.data(), bytes.last(0).data(), count);
    if (parsed.ec != std::errc() || parsed.ptr != bytes.last(0).data() || count == 0) {
      return std::unexpected("--concurrency must be a positive integer");
    }
    options.concurrency = count;
  }
  return {};
}

std::expected<void, std::string> ReadFilterOption(TestCommandOptions& options, Option option) {
  const auto [name, value] = option;
  if (name == "--name") {
    auto pattern = Pattern::Regex(value);
    if (!pattern) {
      return std::unexpected("invalid --name expression: " + pattern.error());
    }
    options.names.push_back(std::move(*pattern));
  } else if (name == "--plain-name") {
    options.plain_names.emplace_back(value);
  } else {
    auto selector = TagSelector::Parse(value);
    if (!selector) {
      return std::unexpected(selector.error());
    }
    if (name == "--tags") {
      options.include_tags = std::move(*selector);
    } else {
      options.exclude_tags = std::move(*selector);
    }
  }
  return {};
}

std::expected<void, std::string> ReadNamedOption(TestCommandOptions& options,
                                                 std::span<const std::string_view> arguments,
                                                 std::size_t& index) {
  constexpr auto kRunnerOptions =
      std::to_array<std::string_view>({"--config", "--timeout", "--reporter", "--concurrency"});
  constexpr auto kFilterOptions =
      std::to_array<std::string_view>({"--name", "--plain-name", "--tags", "--exclude-tags"});
  const auto argument = arguments.subspan(index).front();
  const auto separator = argument.find('=');
  const auto name = argument.substr(0, separator);
  const bool runner = std::ranges::find(kRunnerOptions, name) != kRunnerOptions.end();
  const bool filter = std::ranges::find(kFilterOptions, name) != kFilterOptions.end();
  if (!runner && !filter) {
    return std::unexpected("unknown test option: " + std::string(name));
  }
  std::string_view value;
  if (separator != std::string_view::npos) {
    value = argument.substr(separator + 1);
  } else {
    if (++index == arguments.size()) {
      return std::unexpected(std::string(name) + " requires a value");
    }
    value = arguments.subspan(index).front();
  }
  return runner ? ReadRunnerOption(options, {.name = name, .value = value})
                : ReadFilterOption(options, {.name = name, .value = value});
}

}  // namespace

std::expected<TestCommandOptions, std::string> ParseTestCommandOptions(
    std::span<const std::string_view> arguments) {
  TestCommandOptions options;
  bool positional = false;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto argument = arguments.subspan(index).front();
    if (argument.empty()) {
      return std::unexpected("test path must not be empty");
    }
    if (positional || !argument.starts_with('-')) {
      options.paths.emplace_back(argument);
      continue;
    }
    if (argument == "--") {
      positional = true;
      continue;
    }
    if (argument == "--help" || argument == "-h") {
      options.help = true;
      continue;
    }
    if (argument == "--list") {
      options.list = true;
      continue;
    }
    const auto read = ReadNamedOption(options, arguments, index);
    if (!read) {
      return std::unexpected(read.error());
    }
  }
  return options;
}

void TestCommandOptions::Apply(Settings& settings) const {
  if (timeout) {
    settings.timeout = *timeout;
  }
  if (concurrency) {
    settings.concurrency = *concurrency;
  }
  if (reporter) {
    settings.reporter = *reporter;
  }
  if (include_tags) {
    settings.include_tags = *include_tags;
  }
  if (exclude_tags) {
    settings.exclude_tags = *exclude_tags;
  }
}

std::expected<bool, std::string> TestCommandOptions::IsSelected(std::string_view name) const {
  for (const auto& pattern : names) {
    const auto matched = pattern.IsMatch(name);
    if (!matched) {
      return std::unexpected(matched.error());
    }
    if (!*matched) {
      return false;
    }
  }
  return std::ranges::all_of(plain_names,
                             [name](const auto& filter) { return name.contains(filter); });
}

}  // namespace vela::testing
