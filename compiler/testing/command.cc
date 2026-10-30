#include "compiler/testing/command.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ios>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "base/diagnostic.h"
#include "base/path.h"
#include "base/process_exit.h"
#include "compiler/compilation_cache.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/project_config.h"
#include "compiler/testing/command_options.h"
#include "compiler/testing/discovery.h"
#include "compiler/testing/report.h"
#include "compiler/testing/result.h"
#include "compiler/testing/settings.h"
#include "compiler/testing/suite.h"
#include "compiler/tooling/standard_streams.h"
#include "runtime/future.h"
#include "runtime/isolate.h"
#include "vm/program.h"

namespace vela::testing {
namespace {

// 信号处理只发布无锁标志；VM 和清理在所属 worker 的安全边界观察它。
std::atomic_bool& CancellationFlag() {
  static std::atomic_bool flag{false};
  return flag;
}
static_assert(std::atomic_bool::is_always_lock_free);

void Cancel(int /*signal*/) {
  CancellationFlag().store(true, std::memory_order_relaxed);
}

using SignalHandler = void (*)(int);
SignalHandler InstallCancellationHandler() {
  CancellationFlag().store(false, std::memory_order_relaxed);
  return std::signal(SIGINT, Cancel);
}

class CancellationScope final {
 public:
  CancellationScope() : previous_(InstallCancellationHandler()) {}
  ~CancellationScope() {
    if (IsInstalled()) {
      static_cast<void>(std::signal(SIGINT, previous_));
    }
  }
  [[nodiscard]] bool IsInstalled() const { return previous_ != SIG_ERR; }
  CancellationScope(const CancellationScope&) = delete;
  CancellationScope& operator=(const CancellationScope&) = delete;
  CancellationScope(CancellationScope&&) = delete;
  CancellationScope& operator=(CancellationScope&&) = delete;

 private:
  using Handler = void (*)(int);
  Handler previous_;
};

bool IsCancelled() {
  return CancellationFlag().load(std::memory_order_relaxed);
}

enum class ExitRank : std::uint8_t { Success, TestFailure, Source, Usage, IO, Internal };
ExitRank ExitPriority(int code) {
  switch (code) {
    case kRuntimeErrorExitCode:
      return ExitRank::Internal;
    case kIoErrorExitCode:
      return ExitRank::IO;
    case kUsageExitCode:
      return ExitRank::Usage;
    case kSourceErrorExitCode:
      return ExitRank::Source;
    case 0:
      return ExitRank::Success;
    default:
      return ExitRank::TestFailure;
  }
}

class ScriptOutput final : public std::streambuf {
 public:
  ScriptOutput(SessionReporter& reporter, Suite& suite, std::uint64_t identity, std::string file)
      : reporter_(&reporter), suite_(&suite), identity_(identity), file_(std::move(file)) {}
  void Flush() {
    if (line_.empty()) {
      return;
    }
    Publish();
  }

 protected:
  int_type overflow(int_type character) override {
    if (traits_type::eq_int_type(character, traits_type::eof())) {
      return traits_type::not_eof(character);
    }
    const auto value = traits_type::to_char_type(character);
    if (value == '\n') {
      Publish();
    } else {
      line_ += value;
    }
    return character;
  }
  std::streamsize xsputn(const char* text, std::streamsize size) override {
    for (const char value : std::string_view(text, static_cast<std::size_t>(size))) {
      overflow(value);
    }
    return size;
  }

 private:
  void Publish() {
    ReportEvent event(ReportEventType::Print, identity_);
    event.test = suite_->CurrentTestId();
    event.file = file_;
    event.text = std::move(line_);
    line_.clear();
    reporter_->Emit(event);
  }
  SessionReporter* reporter_;
  Suite* suite_;
  std::uint64_t identity_;
  std::string file_;
  std::string line_;
};

SuiteResult ExecuteSuite(const vm::Program& program,
                         runtime::Isolate& isolate,
                         SuiteOptions options,
                         SessionReporter& reporter) {
  const auto identity = options.suite;
  const auto file = options.file;
  options.emit = [&reporter](const ReportEvent& event) { reporter.Emit(event); };
  std::ostringstream diagnostics;
  Suite suite(isolate, program, std::move(options), diagnostics);
  ScriptOutput buffer(reporter, suite, identity, file);
  std::ostream captured(&buffer);
  auto& previous = isolate.output();
  isolate.SetOutput(captured);
  auto result = suite.Run();
  buffer.Flush();
  isolate.SetOutput(previous);
  return result;
}

void PublishFailure(SessionReporter& reporter,
                    SuiteResult& result,
                    std::uint64_t identity,
                    const std::string& file,
                    TestError error,
                    int code) {
  ReportEvent event(ReportEventType::Error, identity);
  event.file = file;
  event.error = error;
  reporter.Emit(event);
  result.errors.push_back(std::move(error));
  result.exit_code = code;
}

SuiteResult CompileSuite(const std::filesystem::path& path,
                         const compiler::ProjectConfiguration& configuration,
                         const TestCommandOptions& command,
                         std::uint64_t identity,
                         SessionReporter& reporter) {
  SuiteResult result;
  const auto file = PathText(path.lexically_proximate(configuration.root));
  ReportEvent event(ReportEventType::Suite, identity);
  event.file = file;
  reporter.Emit(event);
  SuiteOptions options;
  options.settings = *configuration.test;
  options.list = command.list;
  options.suite = identity;
  options.file = file;
  options.is_cancelled = IsCancelled;
  options.select = [&command](const TestResult& test) { return command.IsSelected(test.name); };
  const auto timeout = options.settings.suite_load_timeout.Apply(kDefaultTimeout);
  if (*timeout) {
    options.load_deadline = runtime::FutureScheduler::Clock::now() + **timeout;
  }
  runtime::Isolate isolate;
  const auto compiled = compiler::CompileFileCached(
      PathText(path), configuration.root / ".cache/vela/test", isolate, configuration.diagnostics);
  if (!compiled) {
    PublishFailure(reporter, result, identity, file,
                   {
                       .phase = TestPhase::Registration,
                       .category = "IOError",
                       .message = compiled.error(),
                       .trace = {},
                       .difference = std::nullopt,
                   },
                   kIoErrorExitCode);
    return result;
  }
  for (const auto& diagnostic : compiled->compiled.diagnostics) {
    if (diagnostic.severity == frontend::DiagnosticSeverity::Warning) {
      ReportEvent warning(ReportEventType::Warning, identity);
      std::ostringstream text;
      frontend::WriteDiagnostic(text, diagnostic, PathText(path));
      warning.file = file;
      warning.text = text.str();
      reporter.Emit(warning);
      continue;
    }
    PublishFailure(
        reporter, result, identity, file,
        {
            .phase = TestPhase::Registration,
            .category = "SourceError",
            .message = diagnostic.message,
            .trace =
                {
                    {
                        .function = {},
                        .source_path = diagnostic.source_path.empty() ? PathText(path)
                                                                      : diagnostic.source_path,
                        .line = diagnostic.location.line,
                        .column = diagnostic.location.column,
                        .instruction = 0,
                        .identity = {},
                    },
                },
            .difference = std::nullopt,
        },
        kSourceErrorExitCode);
  }
  if (!result.errors.empty()) {
    return result;
  }
  if (options.load_deadline && runtime::FutureScheduler::Clock::now() >= *options.load_deadline) {
    PublishFailure(reporter, result, identity, file,
                   {
                       .phase = TestPhase::Registration,
                       .category = "SuiteLoadTimeout",
                       .message = "suite compilation exceeded suite_load_timeout",
                       .trace = {},
                       .difference = std::nullopt,
                   },
                   1);
    return result;
  }
  return ExecuteSuite(compiled->compiled.program, isolate, std::move(options), reporter);
}

enum class RunMode : std::uint8_t { Execute, List };
int Complete(std::span<const SuiteResult> results, SessionReporter& reporter, RunMode mode) {
  ReportEvent done(ReportEventType::Done);
  done.listed = mode == RunMode::List;
  int code = IsCancelled() ? 1 : 0;
  for (const auto& suite : results) {
    if (ExitPriority(suite.exit_code) > ExitPriority(code)) {
      code = suite.exit_code;
    }
    if (!suite.errors.empty() && code == 0) {
      code = 1;
    }
    for (const auto& test : suite.tests) {
      ++done.selected;
      switch (test.status) {
        case TestStatus::Passed:
          ++done.passed;
          break;
        case TestStatus::Failed:
          ++done.failed;
          break;
        case TestStatus::Skipped:
          ++done.skipped;
          break;
        case TestStatus::Cancelled:
          ++done.cancelled;
          break;
        case TestStatus::Pending:
          break;
      }
    }
  }
  if ((done.selected == 0 || done.failed != 0 || done.cancelled != 0) && code == 0) {
    code = 1;
  }
  done.success = code == 0;
  reporter.Emit(done);
  return reporter.HasFailed() && code != kRuntimeErrorExitCode ? kIoErrorExitCode : code;
}

void PrintConfigurationError(std::ostream& errors, const compiler::ConfigurationError& error) {
  WriteError(errors, PathText(error.file), error.location,
             error.field.empty() ? error.message : error.field + ": " + error.message);
}

}  // namespace

bool HasTestModule(const vm::Program& program) {
  return std::ranges::any_of(program.functions(), [](const auto& function) {
    const auto& paths = function.source_paths;
    return paths && std::ranges::find(*paths, "sdk:test") != paths->end();
  });
}

int RunDirectSuite(const vm::Program& program,
                   runtime::Isolate& isolate,
                   std::string_view file,
                   CommandStreams streams) {
  auto& output = streams.output.get();
  auto& errors = streams.errors.get();
  const CancellationScope cancellation;
  if (!cancellation.IsInstalled()) {
    errors << "vela test: cannot install cancellation handler\n";
    return kRuntimeErrorExitCode;
  }
  Settings settings;
  SessionReporter reporter(
      settings.reporter, output,
      tooling::IsInteractiveStandardOutput() ? TerminalMode::Interactive : TerminalMode::Plain);
  reporter.Emit(ReportEvent(ReportEventType::Start));
  ReportEvent event(ReportEventType::Suite, 1);
  event.file = file;
  reporter.Emit(event);
  SuiteOptions options;
  options.file = file;
  options.is_cancelled = IsCancelled;
  const auto result = ExecuteSuite(program, isolate, std::move(options), reporter);
  return Complete(std::span(&result, 1), reporter, RunMode::Execute);
}

int TestCommand(std::span<const std::string_view> arguments, CommandStreams streams) {
  auto& output = streams.output.get();
  auto& errors = streams.errors.get();
  auto command = ParseTestCommandOptions(arguments);
  if (!command) {
    errors << "vela test: " << command.error() << '\n';
    return kUsageExitCode;
  }
  if (command->help) {
    output << "Usage: vela test [paths...] [--config FILE] [--name REGEX] [--plain-name TEXT]\n"
              "  [--tags EXPR] [--exclude-tags EXPR] [--timeout DURATION] [--concurrency N]\n"
              "  [--reporter compact|expanded|json] [--list]\n";
    return 0;
  }
  std::error_code error;
  const auto directory = std::filesystem::current_path(error);
  if (error) {
    errors << "vela test: " << error.message() << '\n';
    return kIoErrorExitCode;
  }
  auto configuration = compiler::LoadProjectConfiguration(directory, command->config);
  if (!configuration) {
    PrintConfigurationError(errors, configuration.error());
    return configuration.error().exit_code;
  }
  if (configuration->test) {
    command->Apply(*configuration->test);
  }
  const auto paths = DiscoverSuites(*configuration, directory, command->paths);
  if (!paths) {
    PrintConfigurationError(errors, paths.error());
    return paths.error().exit_code;
  }
  const auto& settings = *configuration->test;
  const CancellationScope cancellation;
  if (!cancellation.IsInstalled()) {
    errors << "vela test: cannot install cancellation handler\n";
    return kRuntimeErrorExitCode;
  }
  SessionReporter reporter(
      settings.reporter, output,
      tooling::IsInteractiveStandardOutput() ? TerminalMode::Interactive : TerminalMode::Plain);
  reporter.Emit(ReportEvent(ReportEventType::Start));
  std::vector<SuiteResult> results(paths->size());
  std::atomic_size_t next{0};
  std::vector<std::jthread> workers;
  const auto count = std::min<std::size_t>(settings.concurrency, paths->size());
  workers.reserve(count);
  workers.reserve(count);
  for (std::size_t worker = 0; worker < count; ++worker) {
    workers.emplace_back([&] {
      while (!IsCancelled()) {
        const auto index = next.fetch_add(1, std::memory_order_relaxed);
        if (index >= paths->size()) {
          return;
        }
        results.at(index) =
            CompileSuite(paths->at(index), *configuration, *command, index + 1, reporter);
      }
    });
  }
  workers.clear();
  if (paths->empty()) {
    SuiteResult empty;
    PublishFailure(reporter, empty, 0, PathText(configuration->file),
                   {
                       .phase = TestPhase::Registration,
                       .category = "EmptyResult",
                       .message = "no test suites discovered",
                       .trace = {},
                       .difference = std::nullopt,
                   },
                   1);
    results.push_back(std::move(empty));
  }
  return Complete(results, reporter, command->list ? RunMode::List : RunMode::Execute);
}

}  // namespace vela::testing
