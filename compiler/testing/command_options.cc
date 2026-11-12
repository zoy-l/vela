#include "compiler/testing/command_options.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "base/integer_parse.h"
#include "base/path.h"
#include "compiler/testing/pattern.h"
#include "compiler/testing/settings.h"

namespace vela::testing {
namespace {

enum class OptionKind : std::uint8_t {
  Config,
  Timeout,
  Reporter,
  Concurrency,
  Name,
  PlainName,
  Tags,
  ExcludeTags,
};

struct NamedOption final {
  std::string_view name;
  OptionKind kind;
  std::string_view value;
};

constexpr auto kOptions = std::to_array<NamedOption>({
    {.name = "--config", .kind = OptionKind::Config, .value = "FILE"},
    {.name = "--timeout", .kind = OptionKind::Timeout, .value = "DURATION"},
    {.name = "--reporter", .kind = OptionKind::Reporter, .value = "compact|expanded|json"},
    {.name = "--concurrency", .kind = OptionKind::Concurrency, .value = "N"},
    {.name = "--name", .kind = OptionKind::Name, .value = "REGEX"},
    {.name = "--plain-name", .kind = OptionKind::PlainName, .value = "TEXT"},
    {.name = "--tags", .kind = OptionKind::Tags, .value = "EXPR"},
    {.name = "--exclude-tags", .kind = OptionKind::ExcludeTags, .value = "EXPR"},
});

std::expected<void, std::string> ReadOption(TestCommandOptions& options,
                                            OptionKind kind,
                                            std::string_view value) {
  switch (kind) {
    case OptionKind::Config:
      if (value.empty()) {
        return std::unexpected("--config requires a nonempty path");
      }
      options.config = Utf8Path(value);
      return {};
    case OptionKind::Timeout: {
      const auto timeout = ParseTimeout(value);
      if (!timeout) {
        return std::unexpected(timeout.error());
      }
      options.timeout = *timeout;
      return {};
    }
    case OptionKind::Reporter: {
      const auto reporter = ParseReporter(value);
      if (!reporter) {
        return std::unexpected(reporter.error());
      }
      options.reporter = *reporter;
      return {};
    }
    case OptionKind::Concurrency: {
      const auto count = ParseUint32(value);
      if (!count || *count == 0) {
        return std::unexpected("--concurrency must be a positive integer");
      }
      options.concurrency = count;
      return {};
    }
    case OptionKind::Name: {
      auto pattern = Pattern::Regex(value);
      if (!pattern) {
        return std::unexpected("invalid --name expression: " + pattern.error());
      }
      options.names.push_back(std::move(*pattern));
      return {};
    }
    case OptionKind::PlainName:
      options.plain_names.emplace_back(value);
      return {};
    case OptionKind::Tags:
    case OptionKind::ExcludeTags: {
      auto selector = TagSelector::Parse(value);
      if (!selector) {
        return std::unexpected(selector.error());
      }
      if (kind == OptionKind::Tags) {
        options.include_tags = std::move(*selector);
      } else {
        options.exclude_tags = std::move(*selector);
      }
      return {};
    }
  }
  std::unreachable();
}

std::expected<void, std::string> ReadNamedOption(TestCommandOptions& options,
                                                 std::span<const std::string_view> arguments,
                                                 std::size_t& index) {
  const auto argument = arguments[index];
  const auto separator = argument.find('=');
  const auto name = argument.substr(0, separator);
  const auto options_by_name = std::span(kOptions);
  const auto found = std::ranges::find(options_by_name, name, &NamedOption::name);
  if (found == options_by_name.end()) {
    return std::unexpected("unknown test option: " + std::string(name));
  }
  std::string_view value;
  if (separator != std::string_view::npos) {
    value = argument.substr(separator + 1);
  } else {
    if (++index == arguments.size()) {
      return std::unexpected(std::string(name) + " requires a value");
    }
    value = arguments[index];
  }
  return ReadOption(options, found->kind, value);
}

}  // namespace

void WriteTestCommandHelp(std::ostream& output) {
  output << "Usage: vela test [paths...] [options]\n";
  for (const auto& option : kOptions) {
    output << "  " << option.name << ' ' << option.value << '\n';
  }
  output << "  --list\n  --help, -h\n";
}

std::expected<TestCommandOptions, std::string> ParseTestCommandOptions(
    std::span<const std::string_view> arguments) {
  TestCommandOptions options;
  bool positional = false;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
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
