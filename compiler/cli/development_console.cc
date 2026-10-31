#include "compiler/cli/development_console.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <ios>
#include <istream>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <ratio>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "base/path.h"
#include "base/process_exit.h"
#include "compiler/cli/development_debugger.h"
#include "compiler/development/development_files.h"
#include "compiler/development/development_repl.h"
#include "compiler/development/development_session.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "compiler/native/native_roots.h"
#include "compiler/project/project_config.h"
#include "compiler/repl/repl_source.h"
#include "runtime/async/future.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/external_resources.h"
#include "runtime/native/native_control.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/process/process_entry.h"
#include "runtime/value/value_io.h"

namespace vela::tooling {
namespace {

using compiler::DevelopmentOperation;
using compiler::DevelopmentResult;
using compiler::DevelopmentSession;
using compiler::DevelopmentStatus;
using compiler::SourceUpdate;
using Clock = std::chrono::steady_clock;
constexpr auto kPollInterval = std::chrono::milliseconds(25);
constexpr auto kSaveQuietPeriod = std::chrono::milliseconds(100);
constexpr std::string_view kLoadCommand = ".load ";
constexpr std::string_view kCallCommand = ".call ";

std::string_view StatusName(DevelopmentStatus status) {
  switch (status) {
    case DevelopmentStatus::Prepared:
      return "prepared";
    case DevelopmentStatus::WaitingForSafepoint:
      return "waiting for safepoint";
    case DevelopmentStatus::WaitingForRelease:
      return "waiting for host release";
    case DevelopmentStatus::Stopping:
      return "stopping old run";
    case DevelopmentStatus::Accepted:
      return "accepted";
    case DevelopmentStatus::SourceError:
      return "source error";
    case DevelopmentStatus::CodeError:
      return "code generation error";
    case DevelopmentStatus::RequiresReload:
      return "requires Reload";
    case DevelopmentStatus::InstantiationFailed:
      return "resource preparation failed";
    case DevelopmentStatus::Stale:
      return "stale";
    case DevelopmentStatus::Cancelled:
      return "cancelled";
    case DevelopmentStatus::Busy:
      return "another request is pending";
    case DevelopmentStatus::RefreshFailed:
      return "committed; refresh failed";
    case DevelopmentStatus::EntryFailed:
      return "new run installed; entry failed";
    case DevelopmentStatus::InvalidCandidate:
      return "invalid candidate";
  }
  std::unreachable();
}

bool IsSameChanges(const SourceUpdate& left, const SourceUpdate& right) {
  return left.added_roots == right.added_roots &&
         std::ranges::equal(left.changes, right.changes, [](const auto& first, const auto& second) {
           return first.path == second.path && first.text == second.text;
         });
}

bool IsWaiting(DevelopmentStatus status) {
  return status == DevelopmentStatus::WaitingForSafepoint ||
         status == DevelopmentStatus::WaitingForRelease || status == DevelopmentStatus::Stopping;
}

class DevelopmentConsole final {
 public:
  DevelopmentConsole(std::string entry,
                     DevelopmentStreams streams,
                     compiler::jit::CompileOptions compilation)
      : files_(std::move(entry)), streams_(streams), compilation_(std::move(compilation)) {}
  int Run();

 private:
  void Print(const DevelopmentResult& result);
  void Diagnostics(const frontend::Diagnostics& diagnostics) const;
  void ReadCommands();
  void CompileRequests();
  void Prepare(SourceUpdate update, DevelopmentOperation operation);
  void QueueCandidate(compiler::DevelopmentCandidate candidate);
  void ScheduleCandidate();
  void AdvanceCandidate();
  void Command(const std::string& command);
  void OwnerCommand(const std::string& command);
  void Call(const std::string& command);
  void Submit(const std::string& input);
  void Watch();
  void PostDiagnostics(frontend::Diagnostics diagnostics);
  void Post(const std::function<void()>& action);
  void PrintChanged();
  void PollPaused();

  compiler::DevelopmentFiles files_;
  DevelopmentStreams streams_;
  compiler::jit::CompileOptions compilation_;
  std::unique_ptr<DevelopmentSession> session_;
  std::unique_ptr<DevelopmentDebugger> debugger_;
  std::unique_ptr<compiler::DevelopmentRepl> repl_;
  std::string input_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<std::string> commands_;
  std::deque<std::string> calls_;
  std::deque<compiler::DevelopmentCandidate> candidates_;
  bool candidate_scheduled_ = false;
  std::atomic_bool quit_{false};
  std::atomic_bool owner_done_{false};
  bool watching_ = true;
  std::optional<SourceUpdate> observed_;
  std::optional<SourceUpdate> waiting_;
  Clock::time_point saved_;
  std::string last_error_;
  std::uint64_t printed_request_ = 0;
  DevelopmentStatus printed_status_ = DevelopmentStatus::Prepared;
  std::uint64_t failed_entry_generation_ = 0;
};

void DevelopmentConsole::Diagnostics(const frontend::Diagnostics& diagnostics) const {
  for (const auto& diagnostic : diagnostics) {
    frontend::WriteDiagnostic(streams_.errors.get(), diagnostic);
  }
}

void DevelopmentConsole::Print(const DevelopmentResult& result) {
  auto& errors = streams_.errors.get();
  errors << "request " << result.request << ": " << StatusName(result.status) << " (run "
         << result.generation << ", revision " << result.revision << ")";
  if (!result.message.empty()) {
    errors << ": " << result.message;
  }
  errors << '\n';
  Diagnostics(result.diagnostics);
  for (const auto& blocker : result.blockers) {
    errors << "  " << runtime::ExternalResourceKindName(blocker.kind) << " #" << blocker.identity
           << ": " << blocker.references << " reference(s)\n";
  }
  if (result.execution) {
    errors << "  " << ExecutionStatusName(*result.execution) << '\n';
  }
  if (result.status == DevelopmentStatus::Accepted) {
    using Milliseconds = std::chrono::duration<double, std::milli>;
    const auto& times = result.timings;
    errors << "  source " << Milliseconds(times.source).count() << " ms, code "
           << Milliseconds(times.code).count() << " ms, wait "
           << Milliseconds(times.waiting).count() << " ms, pause "
           << Milliseconds(times.pause).count() << " ms\n";
    const auto& native = times.native;
    errors << "  lowering " << Milliseconds(native.generation).count() << " ms, optimization "
           << Milliseconds(native.optimization).count() << " ms, object "
           << Milliseconds(native.compilation).count() << " ms, linking "
           << Milliseconds(native.linking).count() << " ms, cache "
           << Milliseconds(native.cache).count() << " ms\n";
    const auto& publication = times.publication;
    errors << "  instantiation " << Milliseconds(publication.instantiation).count()
           << " ms, validation " << Milliseconds(publication.validation).count()
           << " ms, state preparation " << Milliseconds(publication.state_preparation).count()
           << " ms, deoptimization " << Milliseconds(publication.deoptimization).count()
           << " ms, commit " << Milliseconds(publication.commit).count() << " ms\n";
  }
}

void DevelopmentConsole::PrintChanged() {
  const auto& result = session_->result();
  if (result.request != printed_request_ || result.status != printed_status_) {
    Print(result);
    printed_request_ = result.request;
    printed_status_ = result.status;
  }
  const auto value = session_->entry_value();
  const auto* future = value.IsObject() ? runtime::Heap::GetFuture(*value.AsObject()) : nullptr;
  if (future != nullptr && future->state() == runtime::FutureState::Failed &&
      failed_entry_generation_ != session_->generation()) {
    failed_entry_generation_ = session_->generation();
    const auto diagnostic = future->error_diagnostic();
    if (diagnostic) {
      runtime::WriteRuntimeDiagnostic(streams_.errors.get(), *diagnostic);
    }
  }
}

void DevelopmentConsole::ReadCommands() {
  std::string command;
  while (std::getline(streams_.input.get(), command)) {
    if (command == ".quit" || command == "quit") {
      break;
    }
    {
      const std::scoped_lock lock(mutex_);
      commands_.push_back(std::move(command));
    }
    changed_.notify_all();
  }
  quit_.store(true, std::memory_order_release);
  changed_.notify_all();
  // Reload 可能刚切换运行代次；直到所属线程退出，持续对当前端点请求停止。
  while (!owner_done_.load(std::memory_order_acquire)) {
    session_->safepoint_endpoint().RequestStop();
    std::this_thread::sleep_for(kPollInterval);
  }
}

void DevelopmentConsole::PostDiagnostics(frontend::Diagnostics diagnostics) {
  Post([this, diagnostics = std::move(diagnostics)] { Diagnostics(diagnostics); });
}

void DevelopmentConsole::Post(const std::function<void()>& action) {
  while (!quit_.load(std::memory_order_acquire)) {
    if (session_->safepoint_endpoint().Post(action)) {
      return;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
}

void DevelopmentConsole::Prepare(SourceUpdate update, DevelopmentOperation operation) {
  while (!quit_.load(std::memory_order_acquire)) {
    update.base_revision = session_->snapshot()->revision;
    auto candidate = session_->Prepare(update, operation);
    if (!candidate) {
      if (candidate.error().status == DevelopmentStatus::Stale) {
        continue;
      }
      Post([this, result = std::move(candidate.error())] { Print(result); });
      return;
    }
    if (quit_.load(std::memory_order_acquire)) {
      return;
    }
    auto owned = std::make_shared<compiler::DevelopmentCandidate>(std::move(*candidate));
    Post([this, owned] { QueueCandidate(std::move(*owned)); });
    changed_.notify_all();
    return;
  }
}

void DevelopmentConsole::QueueCandidate(compiler::DevelopmentCandidate candidate) {
  candidates_.push_back(std::move(candidate));
  ScheduleCandidate();
}

void DevelopmentConsole::ScheduleCandidate() {
  if (candidate_scheduled_ || candidates_.empty() || IsWaiting(session_->result().status)) {
    return;
  }
  const auto scheduled = session_->safepoint_endpoint().Post([this] { AdvanceCandidate(); });
  candidate_scheduled_ = scheduled.has_value();
}

void DevelopmentConsole::AdvanceCandidate() {
  candidate_scheduled_ = false;
  if (candidates_.empty() || IsWaiting(session_->result().status)) {
    return;
  }
  auto candidate = std::move(candidates_.front());
  candidates_.pop_front();
  (void)session_->Submit(std::move(candidate));
}

void DevelopmentConsole::OwnerCommand(const std::string& command) {
  auto& errors = streams_.errors.get();
  if (debugger_->Command(command) == DebugCommandResult::Handled) {
    return;
  }
  if (command == ".cancel") {
    const auto cancelled = session_->Cancel();
    if (!cancelled) {
      errors << "request cannot be cancelled in its current phase\n";
    }
  } else if (command == ".status") {
    Print(session_->result());
    const auto code = session_->code_resources();
    const auto layouts = session_->isolate().layout_counts();
    errors << "heap " << session_->isolate().heap().allocated_bytes() << " bytes, " << code.batches
           << " code batch(es), " << code.functions << " function(s), " << layouts.classes
           << " class layout(s)\n";
    errors << "external fields " << session_->isolate().heap().instance_field_bytes() << " bytes, "
           << session_->isolate().heap().instance_type_count() << " live instance type(s)\n";
    errors << "code " << code.executable_bytes << " bytes, read-only image " << code.read_only_bytes
           << " bytes, writable image " << code.writable_bytes << " bytes\n";
    errors << (compilation_.tier == NativeCodeTier::Optimized ? "optimized" : "baseline") << ", "
           << session_->isolate().native_dispatch().deoptimized_frames()
           << " deoptimized frame(s)\n";
  } else if (command == ".gc") {
    session_->isolate().heap().Collect();
    errors << "collection completed\n";
  } else if (command == ".abort") {
    input_.clear();
    errors << "incomplete input cleared\n";
  } else if (command.starts_with(kCallCommand) || command == ".reset" ||
             !command.starts_with('.')) {
    calls_.push_back(command);
  } else {
    errors
        << ".hot  .reload  .load FILE  .call [MODULE] NAME  .watch on|off  .status  .gc  .cancel  "
           ".quit\n.pause  .continue  .step  .stack  .source [FRAME]  .break [MODULE] LINE|clear\n";
    errors << "Enter declarations or statements; expressions echo unless terminated by ';'.\n"
              ".abort clears incomplete input; .reset rebuilds the run and clears REPL history.\n";
  }
}

void DevelopmentConsole::Call(const std::string& command) {
  std::istringstream arguments(command.substr(kCallCommand.size()));
  std::string first;
  std::string second;
  arguments >> first >> second;
  const auto module_uri = second.empty() ? session_->snapshot()->roots.front() : first;
  const auto& name = second.empty() ? first : second;
  const auto value = session_->Invoke(module_uri, name);
  if (value) {
    runtime::WriteValue(streams_.output.get(), *value);
    streams_.output.get() << '\n';
  } else {
    if (const auto& diagnostic = session_->last_diagnostic()) {
      runtime::WriteRuntimeDiagnostic(streams_.errors.get(), *diagnostic);
    } else {
      streams_.errors.get() << "call failed: " << ExecutionStatusName(value.error()) << '\n';
    }
  }
}

void DevelopmentConsole::Submit(const std::string& input) {
  if (input == ".reset") {
    auto update = files_.Capture(*session_->snapshot());
    if (!update) {
      Diagnostics(update.error());
      return;
    }
    const auto result = repl_->Reset(std::move(*update));
    if (result.request != printed_request_ || result.status != printed_status_) {
      Print(result);
    }
    if (result.status == DevelopmentStatus::Accepted ||
        result.status == DevelopmentStatus::EntryFailed) {
      input_.clear();
    }
    return;
  }
  input_ += input;
  input_ += '\n';
  const auto result = repl_->Submit(input_);
  if (result.status == compiler::SubmissionStatus::Incomplete) {
    streams_.errors.get() << "incomplete submission; continue input or use .abort\n";
    return;
  }
  input_.clear();
  Diagnostics(result.diagnostics);
  if (result.update && result.status == compiler::SubmissionStatus::Rejected) {
    Print(*result.update);
  }
  if (result.runtime_diagnostic) {
    runtime::WriteRuntimeDiagnostic(streams_.errors.get(), *result.runtime_diagnostic);
  } else if (result.execution) {
    streams_.errors.get() << "submission failed: " << ExecutionStatusName(*result.execution)
                          << '\n';
  }
}

void DevelopmentConsole::Command(const std::string& command) {
  if (command == ".watch on" || command == ".watch off") {
    watching_ = command == ".watch on";
    return;
  }
  if (command == ".hot" || command == ".reload" || command.starts_with(kLoadCommand)) {
    const auto previous = session_->snapshot();
    auto update =
        command.starts_with(kLoadCommand)
            ? compiler::DevelopmentFiles(command.substr(kLoadCommand.size())).Capture(*previous)
            : files_.Capture(*previous);
    if (!update) {
      PostDiagnostics(std::move(update.error()));
      return;
    }
    auto operation = DevelopmentOperation::Load;
    if (command == ".reload") {
      operation = DevelopmentOperation::Reload;
    } else if (command == ".hot") {
      operation = DevelopmentOperation::HotReload;
    }
    Prepare(std::move(*update), operation);
    return;
  }
  Post([this, command] { OwnerCommand(command); });
}

void DevelopmentConsole::Watch() {
  auto update = files_.Capture(*session_->snapshot());
  if (!update) {
    std::ostringstream signature;
    for (const auto& diagnostic : update.error()) {
      signature << diagnostic.source_path << ':' << diagnostic.location.line << ':'
                << diagnostic.message;
    }
    const auto error = signature.str();
    if (error != last_error_) {
      last_error_ = error;
      PostDiagnostics(std::move(update.error()));
    }
    waiting_.reset();
    return;
  }
  last_error_.clear();
  if (update->changes.empty() && update->added_roots.empty()) {
    waiting_.reset();
    return;
  }
  if (!observed_ || !IsSameChanges(*observed_, *update)) {
    observed_ = *update;
    waiting_ = std::move(*update);
    saved_ = Clock::now();
  } else if (waiting_ && Clock::now() - saved_ >= kSaveQuietPeriod) {
    auto ready = std::move(*waiting_);
    waiting_.reset();
    Prepare(std::move(ready), DevelopmentOperation::HotReload);
  }
}

void DevelopmentConsole::CompileRequests() {
  while (!quit_.load(std::memory_order_acquire)) {
    std::optional<std::string> command;
    {
      std::unique_lock lock(mutex_);
      changed_.wait_for(lock, kPollInterval, [this] { return !commands_.empty() || quit_.load(); });
      if (!commands_.empty()) {
        command = std::move(commands_.front());
        commands_.pop_front();
      }
    }
    if (command) {
      Command(*command);
    } else if (watching_) {
      Watch();
    }
  }
}

int DevelopmentConsole::Run() {
  const compiler::CompiledSourceSnapshot empty;
  auto update = files_.Capture(empty);
  if (!update) {
    Diagnostics(update.error());
    return kSourceErrorExitCode;
  }
  compiler::DevelopmentOptions options;
  const auto diagnostics = compiler::LoadSourceDiagnosticOptions(Utf8Path(files_.entry()));
  if (!diagnostics) {
    frontend::WriteDiagnostic(streams_.errors.get(),
                              compiler::ConfigurationDiagnostic(diagnostics.error()));
    return diagnostics.error().exit_code;
  }
  options.diagnostics = *diagnostics;
  options.compilation = compilation_;
  options.compilation.debugging = compiler::native::NativeDebugging::SourceLines;
  options.progress = [this](const DevelopmentResult& result) {
    Print(result);
    const auto& current = session_->result();
    printed_request_ = current.request;
    printed_status_ = current.status;
    ScheduleCandidate();
  };
  auto created = DevelopmentSession::Create(files_.entry(), {}, std::move(options));
  if (!created) {
    Diagnostics(created.error());
    return kSourceErrorExitCode;
  }
  session_ = std::move(*created);
  debugger_ = std::make_unique<DevelopmentDebugger>(*session_, streams_.errors.get(),
                                                    [this] { PollPaused(); });
  repl_ = std::make_unique<compiler::DevelopmentRepl>(
      *session_, PathText(Utf8Path(files_.entry()).parent_path() / "__vela_dev_repl__.vela"));
  session_->isolate().SetOutput(streams_.output.get());
  auto candidate = session_->Prepare(std::move(*update), DevelopmentOperation::Load);
  if (!candidate) {
    Print(candidate.error());
    return kSourceErrorExitCode;
  }
  (void)session_->Submit(std::move(*candidate));
  const auto loaded = session_->Pump();
  if (loaded.status != DevelopmentStatus::Accepted) {
    return kSourceErrorExitCode;
  }
  printed_request_ = loaded.request;
  printed_status_ = loaded.status;
  auto& output = streams_.output.get();
  const auto output_flags = output.flags();
  output.setf(std::ios::unitbuf);
  streams_.errors.get() << "Vela JIT development session (.help for commands; watching files)\n";
  std::thread input([this] { ReadCommands(); });
  std::thread compiler([this] { CompileRequests(); });
  const auto entry = session_->RunEntry();
  if (!entry && entry.error() != ExecutionStatus::ExecutionStopped) {
    streams_.errors.get() << "entry failed: " << ExecutionStatusName(entry.error()) << '\n';
  }
  while (!quit_.load(std::memory_order_acquire)) {
    (void)session_->Pump();
    if (!calls_.empty() && candidates_.empty() && !candidate_scheduled_ &&
        !IsWaiting(session_->result().status)) {
      auto command = std::move(calls_.front());
      calls_.pop_front();
      if (command.starts_with(kCallCommand)) {
        Call(command);
      } else {
        Submit(command);
      }
    }
    (void)session_->isolate().scheduler().PumpOne();
    PrintChanged();
    std::unique_lock lock(mutex_);
    changed_.wait_for(lock, kPollInterval);
  }
  owner_done_.store(true, std::memory_order_release);
  changed_.notify_all();
  input.join();
  compiler.join();
  output.flags(output_flags);
  return streams_.input.get().bad() ? kIoErrorExitCode : 0;
}

void DevelopmentConsole::PollPaused() {
  (void)session_->Pump();
  PrintChanged();
  std::unique_lock lock(mutex_);
  changed_.wait_for(lock, kPollInterval);
}

}  // namespace

int RunDevelopmentConsole(std::string entry,
                          DevelopmentStreams streams,
                          compiler::jit::CompileOptions compilation) {
  return DevelopmentConsole(std::move(entry), streams, std::move(compilation)).Run();
}

}  // namespace vela::tooling
