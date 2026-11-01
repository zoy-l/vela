#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <benchmark/benchmark.h>

#include "base/file.h"
#include "base/path.h"
#include "benchmarks/temporary_directory.h"
#include "compiler/development/development_files.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"

namespace vela::benchmarks {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kFileEventTimeout = std::chrono::seconds(5);
constexpr std::array<std::int64_t, 4> kProjectSizes{2, 32, 256, 2048};
enum class FileActivity : std::uint8_t { Idle, Edit };

std::string ValueSource(std::int64_t value) {
  return "export func value(): int { return " + std::to_string(value) + "; }";
}

std::expected<void, std::string> WriteProject(const std::filesystem::path& directory,
                                              std::int64_t files) {
  std::string entry;
  for (std::int64_t index = 1; index < files; ++index) {
    const auto name = "module_" + std::to_string(index);
    entry.append("import '").append(name).append(".vela' as ").append(name).append("; ");
    const auto text = ValueSource(index);
    const auto written =
        WriteFileAtomically(directory / (name + ".vela"), std::as_bytes(std::span(text)));
    if (!written) {
      return written;
    }
  }
  entry += "export func value(): int { return module_1.value(); }";
  return WriteFileAtomically(directory / "main.vela", std::as_bytes(std::span(entry)));
}

std::expected<std::unique_ptr<compiler::SourceSession>, std::string> OpenSource(
    compiler::DevelopmentFiles& files) {
  if (const auto watched = files.StartWatching(); !watched) {
    return std::unexpected(frontend::DiagnosticsText(watched.error()));
  }
  const compiler::CompiledSourceSnapshot empty;
  auto captured = files.Capture(empty);
  if (!captured) {
    return std::unexpected(frontend::DiagnosticsText(captured.error()));
  }
  auto source = std::make_unique<compiler::SourceSession>(files.entry());
  auto initial = source->Prepare(std::move(*captured));
  if (!initial) {
    return std::unexpected(frontend::DiagnosticsText(initial.error().diagnostics));
  }
  if (!source->Commit(*initial)) {
    return std::unexpected("initial file capture could not be committed");
  }
  if (const auto accepted = files.Accept(source->snapshot()); !accepted) {
    return std::unexpected(frontend::DiagnosticsText(accepted.error()));
  }
  if (const auto polled = files.Poll(); !polled) {
    return std::unexpected(frontend::DiagnosticsText(polled.error()));
  }
  return source;
}

void PollIdle(benchmark::State& state, compiler::DevelopmentFiles& files) {
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    const auto polled = files.Poll();
    if (!polled) {
      state.SkipWithError(frontend::DiagnosticsText(polled.error()));
      return;
    }
    if (*polled) {
      state.SkipWithError("idle file capture produced an update");
      return;
    }
    benchmark::DoNotOptimize(polled);
  }
}

std::expected<Clock::duration, std::string> CaptureEdit(compiler::DevelopmentFiles& files,
                                                        compiler::SourceSession& source,
                                                        const std::string& path,
                                                        std::string_view text) {
  const auto started = Clock::now();
  while (true) {
    auto polled = files.Poll(kFileEventTimeout);
    if (!polled) {
      return std::unexpected(frontend::DiagnosticsText(polled.error()));
    }
    const auto elapsed = Clock::now() - started;
    if (elapsed > kFileEventTimeout) {
      return std::unexpected("file event did not arrive");
    }
    if (!*polled || (**polled).changes.empty()) {
      continue;
    }
    auto& update = **polled;
    if (update.changes.size() != 1 || update.changes.front().path != path ||
        update.changes.front().text != text || !update.added_roots.empty()) {
      return std::unexpected("file capture did not isolate the edited module");
    }
    // 类型检查和提交验证捕获结果，但不计入文件事件与读取阶段的耗时。
    auto candidate = source.Prepare(std::move(update));
    if (!candidate) {
      return std::unexpected(frontend::DiagnosticsText(candidate.error().diagnostics));
    }
    if (!source.Commit(*candidate)) {
      return std::unexpected("edited file capture could not be committed");
    }
    if (const auto accepted = files.Accept(source.snapshot()); !accepted) {
      return std::unexpected(frontend::DiagnosticsText(accepted.error()));
    }
    return elapsed;
  }
}

void CaptureFiles(benchmark::State& state, FileActivity activity) {
  BenchmarkDirectory directory(state);
  if (!directory.IsValid()) {
    return;
  }
  const auto count = state.range(0);
  if (const auto written = WriteProject(directory.path(), count); !written) {
    state.SkipWithError(written.error());
    return;
  }
  compiler::DevelopmentFiles files(PathText(directory.path() / "main.vela"));
  auto opened = OpenSource(files);
  if (!opened) {
    state.SkipWithError(opened.error());
    return;
  }
  auto& source = **opened;
  state.counters["files"] = static_cast<double>(count);
  state.counters["changed_files"] = activity == FileActivity::Idle ? 0 : 1;
  if (activity == FileActivity::Idle) {
    PollIdle(state, files);
    return;
  }
  const auto edited = directory.path() / "module_1.vela";
  const auto path = PathText(edited);
  std::int64_t version = count;
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    const auto text = ValueSource(++version);
    if (const auto written = WriteFileAtomically(edited, std::as_bytes(std::span(text)));
        !written) {
      state.SkipWithError(written.error());
      return;
    }
    const auto elapsed = CaptureEdit(files, source, path, text);
    if (!elapsed) {
      state.SkipWithError(elapsed.error());
      return;
    }
    state.SetIterationTime(std::chrono::duration<double>(*elapsed).count());
  }
}

int Register() {
  for (const auto activity : {FileActivity::Idle, FileActivity::Edit}) {
    const auto* const name =
        activity == FileActivity::Idle ? "files/capture/idle" : "files/capture/edit";
    auto* registration = benchmark::RegisterBenchmark(name, CaptureFiles, activity);
    for (const auto size : kProjectSizes) {
      registration->Arg(size);
    }
    if (activity == FileActivity::Idle) {
      registration->UseRealTime();
    } else {
      registration->UseManualTime();
    }
  }
  return 0;
}

[[maybe_unused]] const int kRegistered = Register();

}  // namespace
}  // namespace vela::benchmarks
