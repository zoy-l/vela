#include "compiler/development_session.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/path.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"
#include "compiler/jit/code_resources.h"
#include "compiler/jit/compiler.h"
#include "compiler/source_session.h"
#include "runtime/external_resources.h"
#include "runtime/isolate.h"
#include "runtime/native_abi.h"
#include "runtime/native_call.h"
#include "runtime/native_control.h"
#include "runtime/native_dispatch.h"
#include "runtime/native_instance.h"
#include "runtime/value.h"

namespace vela::compiler {
namespace {

using Clock = std::chrono::steady_clock;

void AddTimings(DevelopmentTimings& total, const DevelopmentTimings& addition) {
  total.source += addition.source;
  total.code += addition.code;
  total.preparation += addition.preparation;
  total.waiting += addition.waiting;
  total.pause += addition.pause;
  total.stopping += addition.stopping;
  total.refresh += addition.refresh;
  total.entry += addition.entry;
  total.native.generation += addition.native.generation;
  total.native.cache += addition.native.cache;
  total.native.optimization += addition.native.optimization;
  total.native.compilation += addition.native.compilation;
  total.native.linking += addition.native.linking;
  total.publication += addition.publication;
  total.frontend.loading += addition.frontend.loading;
  total.frontend.checking += addition.frontend.checking;
  total.frontend.ir += addition.frontend.ir;
}

DevelopmentStatus PublicationStatus(runtime::NativePublicationStatus status) {
  using Status = runtime::NativePublicationStatus;
  switch (status) {
    case Status::Stale:
    case Status::NeedsCompilation:
      return DevelopmentStatus::Stale;
    case Status::Busy:
      return DevelopmentStatus::WaitingForSafepoint;
    case Status::RequiresReload:
      return DevelopmentStatus::RequiresReload;
    case Status::Stopped:
    case Status::InvalidIdentity:
    case Status::InstantiationFailed:
      return DevelopmentStatus::InstantiationFailed;
  }
  std::unreachable();
}

void PublicationError(DevelopmentResult& result, const runtime::NativePublicationError& error) {
  result.status = PublicationStatus(error.status);
  result.message = error.message;
  result.declaration = error.declaration;
  result.dependency = error.dependency;
}

std::expected<runtime::NativePublicationTimings, runtime::NativePublicationError> PublishCode(
    runtime::NativeDispatch& dispatch,
    std::uint64_t revision,
    const std::shared_ptr<const runtime::NativeImage>& image) {
  auto candidate = dispatch.Prepare(revision, image);
  if (!candidate) {
    return std::unexpected(std::move(candidate.error()));
  }
  return dispatch.Publish(std::move(*candidate));
}

DevelopmentChanges Changes(const CompiledSourceSnapshot& previous,
                           const CompiledSourceSnapshot& candidate,
                           const SourceUpdate& update) {
  DevelopmentChanges changes;
  for (const auto& module : update.changes) {
    changes.modules.push_back(module.path);
  }
  absl::flat_hash_set<DeclarationId> before;
  for (const auto& declaration : previous.program.declarations) {
    before.insert(declaration.identity);
  }
  for (const auto& declaration : candidate.program.declarations) {
    if (before.erase(declaration.identity) == 0) {
      ++changes.added_declarations;
    }
  }
  changes.removed_declarations = before.size();
  changes.compiled_functions = candidate.program.functions.size();
  return changes;
}

SourceUpdate FullUpdate(const CompiledSourceSnapshot& previous, const SourceUpdate& update) {
  absl::flat_hash_map<std::string, SourceChange> sources;
  for (const auto& [path, text] : previous.sources) {
    sources.emplace(path, SourceChange{
                              .path = path,
                              .text = *text,
                              .syntax = previous.prepared_sources.contains(path)
                                            ? previous.modules.sources.at(path)
                                            : nullptr,
                          });
  }
  for (const auto& change : update.changes) {
    const auto path = PathText(Utf8Path(change.path).lexically_normal());
    if (change.text) {
      sources.insert_or_assign(path, change);
    } else {
      sources.erase(path);
    }
  }
  SourceUpdate full;
  for (auto& [path, change] : sources) {
    full.changes.push_back(std::move(change));
  }
  for (const auto& root : previous.roots) {
    if (sources.contains(root)) {
      full.added_roots.push_back(root);
    }
  }
  full.added_roots.insert(full.added_roots.end(), update.added_roots.begin(),
                          update.added_roots.end());
  return full;
}

}  // namespace

DevelopmentCandidate::DevelopmentCandidate(
    std::shared_ptr<const DevelopmentSessionIdentity> session,
    SourceUpdate update,
    SourceCandidate source,
    jit::CompiledImage code,
    std::unique_ptr<SourceSession> replacement,
    DevelopmentResult result)
    : session_(std::move(session)),
      update_(std::move(update)),
      source_(std::move(source)),
      code_(std::move(code)),
      replacement_(std::move(replacement)),
      result_(std::move(result)) {}

DevelopmentSession::Pending::Pending(DevelopmentCandidate source)
    : candidate(std::move(source)), queued(Clock::now()) {}

DevelopmentSession::DevelopmentSession(std::string entry,
                                       runtime::NativeBindings bindings,
                                       DevelopmentOptions options,
                                       std::unique_ptr<jit::Compiler> compiler)
    : identity_(std::make_shared<const DevelopmentSessionIdentity>()),
      entry_(std::move(entry)),
      bindings_(std::move(bindings)),
      options_(std::move(options)),
      compiler_(std::move(compiler)),
      sources_(std::make_shared<SourceSession>(entry_, options_.diagnostics)),
      isolate_(std::make_unique<runtime::Isolate>(bindings_, options_.gc_threshold)),
      endpoint_(isolate_->native_control().endpoint()) {
  isolate_->native_control().SetDebugger(options_.debugger);
}

DevelopmentSession::~DevelopmentSession() {
  isolate_->native_control().CancelSafepoint();
}

std::expected<std::unique_ptr<DevelopmentSession>, frontend::Diagnostics>
DevelopmentSession::Create(std::string entry,
                           runtime::NativeBindings bindings,
                           DevelopmentOptions options) {
  auto compiler = jit::Compiler::Create();
  if (!compiler) {
    return std::unexpected(std::move(compiler.error()));
  }
  return std::unique_ptr<DevelopmentSession>(new DevelopmentSession(
      std::move(entry), std::move(bindings), std::move(options), std::move(*compiler)));
}

std::shared_ptr<const CompiledSourceSnapshot> DevelopmentSession::snapshot() const noexcept {
  std::shared_ptr<SourceSession> sources;
  {
    const std::scoped_lock lock(source_mutex_);
    sources = sources_;
  }
  return sources->snapshot();
}

std::uint64_t DevelopmentSession::generation() const noexcept {
  const std::scoped_lock lock(source_mutex_);
  return generation_;
}

jit::CodeResourceCounts DevelopmentSession::code_resources() const noexcept {
  return compiler_->resource_counts();
}

DevelopmentSession::CompilationBase DevelopmentSession::BeginCompilation() {
  const std::scoped_lock lock(source_mutex_);
  return {.sources = sources_, .generation = generation_, .request = next_request_++};
}

std::expected<DevelopmentCandidate, DevelopmentResult> DevelopmentSession::Prepare(
    SourceUpdate update,
    DevelopmentOperation operation) {
  return Prepare(std::move(update), operation, {});
}

std::expected<DevelopmentCandidate, DevelopmentResult> DevelopmentSession::Prepare(
    SourceUpdate update,
    DevelopmentOperation operation,
    std::span<const DeclarationId> retained) {
  const auto base = BeginCompilation();
  const auto previous = base.sources->snapshot();
  DevelopmentResult result;
  result.operation = operation;
  result.request = base.request;
  result.generation = base.generation;
  result.revision = previous->revision;
  if (update.base_revision != previous->revision) {
    result.status = DevelopmentStatus::Stale;
    result.message = "source request baseline is stale";
    return std::unexpected(std::move(result));
  }
  if (operation == DevelopmentOperation::Load) {
    for (const auto& change : update.changes) {
      const auto path = PathText(Utf8Path(change.path).lexically_normal());
      if (!change.text || previous->sources.contains(path)) {
        result.status = DevelopmentStatus::SourceError;
        result.message = "Load only adds modules; edit existing modules with Hot Reload or Reload";
        return std::unexpected(std::move(result));
      }
    }
  }
  const auto started = Clock::now();
  std::unique_ptr<SourceSession> replacement;
  if (operation == DevelopmentOperation::Reload) {
    replacement = std::make_unique<SourceSession>(entry_, options_.diagnostics);
  }
  auto source = replacement ? replacement->Prepare(FullUpdate(*previous, update))
                            : base.sources->Prepare(update, retained);
  result.timings.source = Clock::now() - started;
  if (!source) {
    switch (source.error().status) {
      case SourceCandidateStatus::Stale:
        result.status = DevelopmentStatus::Stale;
        break;
      case SourceCandidateStatus::RequiresReload:
        result.status = DevelopmentStatus::RequiresReload;
        break;
      case SourceCandidateStatus::SourceError:
        result.status = DevelopmentStatus::SourceError;
        break;
      case SourceCandidateStatus::DifferentSession:
        result.status = DevelopmentStatus::InvalidCandidate;
        break;
    }
    result.diagnostics = std::move(source.error().diagnostics);
    result.declaration = source.error().declaration;
    return std::unexpected(std::move(result));
  }
  const auto code_started = Clock::now();
  auto code = compiler_->Compile(source->snapshot().program, options_.compilation);
  result.timings.code = Clock::now() - code_started;
  if (!code) {
    result.status = DevelopmentStatus::CodeError;
    result.diagnostics = std::move(code.error());
    return std::unexpected(std::move(result));
  }
  result.timings.native = code->timings;
  result.diagnostics = source->snapshot().diagnostics;
  result.timings.frontend = source->snapshot().timings;
  result.cache = code->cache;
  result.changes = Changes(*previous, source->snapshot(), update);
  return DevelopmentCandidate(identity_, std::move(update), std::move(*source), std::move(*code),
                              std::move(replacement), std::move(result));
}

DevelopmentResult DevelopmentSession::Submit(DevelopmentCandidate candidate) {
  if (pending_ != nullptr) {
    auto result = candidate.result_;
    result.status = DevelopmentStatus::Busy;
    result.message = "another development operation is pending";
    Report(result);
    return result;
  }
  if (candidate.session_ != identity_) {
    auto result = candidate.result_;
    result.status = DevelopmentStatus::InvalidCandidate;
    result.message = "candidate belongs to another development session";
    Report(result);
    return result;
  }
  if (candidate.result_.generation != generation() ||
      candidate.update_.base_revision != snapshot()->revision) {
    const auto original = candidate.result_;
    candidate.update_.base_revision = snapshot()->revision;
    auto prepared = Prepare(std::move(candidate.update_), candidate.result_.operation);
    if (!prepared) {
      result_ = std::move(prepared.error());
      result_.request = original.request;
      AddTimings(result_.timings, original.timings);
      Report(result_);
      return result_;
    }
    prepared->result_.request = original.request;
    AddTimings(prepared->result_.timings, original.timings);
    candidate = std::move(*prepared);
  }
  result_ = candidate.result_;
  pending_ = std::make_unique<Pending>(std::move(candidate));
  if (result_.operation == DevelopmentOperation::Reload) {
    const auto started = Clock::now();
    auto& replacement = pending_->replacement;
    replacement = std::make_unique<runtime::Isolate>(bindings_, options_.gc_threshold);
    replacement->SetOutput(isolate_->output());
    auto& dispatch = replacement->native_dispatch();
    const auto published = PublishCode(dispatch, 0, pending_->candidate.code_.image);
    result_.timings.preparation += Clock::now() - started;
    if (!published) {
      PublicationError(result_, published.error());
      pending_.reset();
      Report(result_);
      return result_;
    }
    result_.timings.publication += *published;
  }
  const auto scheduled = isolate_->native_control().ScheduleSafepoint([this] { AtSafepoint(); });
  if (!scheduled) {
    result_.status = DevelopmentStatus::Busy;
    result_.message = "the Isolate has another safepoint action or has stopped";
    pending_.reset();
    Report(result_);
    return result_;
  }
  result_.status = DevelopmentStatus::WaitingForSafepoint;
  Report(result_);
  return result_;
}

runtime::NativeSafepointEndpoint DevelopmentSession::safepoint_endpoint() const {
  const std::scoped_lock lock(source_mutex_);
  return endpoint_;
}

void DevelopmentSession::SetDebugger(runtime::NativeDebugger* debugger) noexcept {
  options_.debugger = debugger;
  isolate_->native_control().SetDebugger(debugger);
}

std::expected<void, runtime::NativeSafepointError> DevelopmentSession::Post(
    DevelopmentCandidate candidate,
    std::function<void(const DevelopmentResult&)> submitted) {
  return safepoint_endpoint().Post(
      [this, candidate = std::move(candidate), submitted = std::move(submitted)]() mutable {
        const auto result = Submit(std::move(candidate));
        if (submitted) {
          submitted(result);
        }
      });
}

void DevelopmentSession::AtSafepoint() {
  result_.timings.waiting += Clock::now() - pending_->queued;
  if (result_.operation == DevelopmentOperation::Reload) {
    BeginReload();
  } else {
    Publish();
  }
}

void DevelopmentSession::Publish() {
  const auto started = Clock::now();
  auto& dispatch = isolate_->native_dispatch();
  const auto published =
      PublishCode(dispatch, snapshot()->revision, pending_->candidate.code_.image);
  if (!published) {
    result_.timings.pause += Clock::now() - started;
    PublicationError(result_, published.error());
    // 编译期间出现的存留特化需求属于候选过期，可按同一不可变源码请求重新编译。
    if (published.error().status == runtime::NativePublicationStatus::NeedsCompilation) {
      const auto original = result_;
      Report(result_);
      const auto recompiling = Clock::now();
      auto update = pending_->candidate.update_;
      const auto operation = result_.operation;
      pending_.reset();
      isolate_->heap().Collect();
      const auto retained = isolate_->native_dispatch().RetainedDeclarations();
      auto candidate = Prepare(std::move(update), operation, retained);
      if (!candidate) {
        result_ = std::move(candidate.error());
        result_.request = original.request;
        AddTimings(result_.timings, original.timings);
        result_.timings.pause += Clock::now() - recompiling;
        Report(result_);
        return;
      }
      candidate->result_.request = original.request;
      AddTimings(candidate->result_.timings, original.timings);
      candidate->result_.timings.pause += Clock::now() - recompiling;
      (void)Submit(std::move(*candidate));
      return;
    }
    pending_.reset();
    Report(result_);
    return;
  }
  result_.timings.publication += *published;
  [[maybe_unused]] const auto committed = sources_->Commit(pending_->candidate.source_);
  // 源码提交只由本会话执行；发布临界区没有用户回调，不可能改变其基线。
  assert(committed.has_value());
  result_.revision = snapshot()->revision;
  result_.status = DevelopmentStatus::Accepted;
  if (result_.operation == DevelopmentOperation::HotReload) {
    result_.message = "state retained; the entry and completed initializers were not rerun";
  }
  pending_.reset();
  result_.timings.pause += Clock::now() - started;
  Report(result_);
  Notify();
}

std::vector<runtime::ExternalResourceInfo> DevelopmentSession::HostBlockers() const {
  auto blockers = isolate_->ReloadBlockers();
  std::erase_if(blockers, [](const auto& blocker) {
    return blocker.kind == runtime::ExternalResourceKind::Execution;
  });
  return blockers;
}

void DevelopmentSession::BeginReload() {
  const auto previous = result_.status;
  result_.blockers = HostBlockers();
  if (!result_.blockers.empty()) {
    result_.status = DevelopmentStatus::WaitingForRelease;
    result_.message = "release external roots and handles before Reload";
    if (previous != result_.status) {
      Report(result_);
    }
    return;
  }
  pending_->stopping = Clock::now();
  result_.status = DevelopmentStatus::Stopping;
  result_.message.clear();
  entry_root_.Detach();
  entry_value_ = runtime::Value::Null();
  // 请求只在最外层宿主借用和初始化结束后到达这里；专用停止状态逐层退出旧原生帧。
  isolate_->native_control().RequestStop();
  isolate_->native_dispatch().Close();
  isolate_->scheduler().Shutdown();
  Report(result_);
}

void DevelopmentSession::FinishReload() {
  if (isolate_->native_dispatch().HasExecutions() ||
      isolate_->native_control().IsProcessingSafepoint()) {
    return;
  }
  auto stopped = isolate_->TryStop();
  if (!stopped) {
    const auto first = result_.blockers.empty();
    result_.blockers = std::move(stopped.error());
    result_.message = "old execution stopped; external handles must be released before switching";
    if (first) {
      Report(result_);
    }
    return;
  }
  const auto stopping = pending_->stopping;
  auto& candidate = pending_->candidate;
  [[maybe_unused]] const auto committed = candidate.replacement_->Commit(candidate.source_);
  assert(committed.has_value());
  isolate_ = std::move(pending_->replacement);
  isolate_->native_control().SetDebugger(options_.debugger);
  {
    const std::scoped_lock lock(source_mutex_);
    endpoint_ = isolate_->native_control().endpoint();
    sources_ = std::move(candidate.replacement_);
    result_.generation = ++generation_;
  }
  result_.revision = snapshot()->revision;
  result_.blockers.clear();
  result_.message.clear();
  result_.status = DevelopmentStatus::Accepted;
  pending_.reset();
  result_.timings.stopping += Clock::now() - stopping;
  Report(result_);
  const auto started = Clock::now();
  const auto entry = RunEntry();
  result_.timings.entry += Clock::now() - started;
  if (!entry) {
    result_.status = DevelopmentStatus::EntryFailed;
    result_.execution = entry.error();
    result_.message = "new run was installed but its entry failed";
    Report(result_);
  }
}

void DevelopmentSession::Notify() {
  if (!options_.refreshed) {
    return;
  }
  const auto accepted = result_;
  const auto started = Clock::now();
  const auto refreshed = options_.refreshed(*this, accepted);
  result_.timings.refresh += Clock::now() - started;
  if (!refreshed) {
    result_.status = DevelopmentStatus::RefreshFailed;
    result_.execution = refreshed.error();
    result_.message = "code committed; refresh failed";
    Report(result_);
  }
}

void DevelopmentSession::Report(const DevelopmentResult& result) const {
  if (options_.progress) {
    options_.progress(result);
  }
}

DevelopmentResult DevelopmentSession::Pump() {
  const auto& control = isolate_->native_control();
  if (control.HasHostBorrow()) {
    return result_;
  }
  if (pending_ != nullptr && result_.status == DevelopmentStatus::WaitingForRelease &&
      control.CanPublish()) {
    BeginReload();
  }
  if (!control.IsStopRequested()) {
    (void)isolate_->native_control().ProcessSafepoint();
  }
  if (pending_ != nullptr && result_.status == DevelopmentStatus::Stopping) {
    FinishReload();
  }
  return result_;
}

std::expected<void, DevelopmentStatus> DevelopmentSession::Cancel() {
  if (pending_ == nullptr || result_.status == DevelopmentStatus::Stopping) {
    return std::unexpected(result_.status);
  }
  isolate_->native_control().CancelSafepoint();
  pending_.reset();
  result_.status = DevelopmentStatus::Cancelled;
  Report(result_);
  return {};
}

std::expected<runtime::Value, ExecutionStatus> DevelopmentSession::InvokeDeclaration(
    DeclarationId declaration,
    std::span<const runtime::Value> arguments,
    Invocation invocation) {
  const auto target = isolate_->native_dispatch().Resolve(declaration);
  if (!target) {
    return std::unexpected(target.error());
  }
  std::expected<runtime::Value, ExecutionStatus> result;
  last_diagnostic_.reset();
  const auto entered_generation = generation();
  {
    auto instance = runtime::NativeInstance::Borrow(*target->state);
    runtime::NativeExecution execution(instance);
    result = invocation == Invocation::Entry
                 ? runtime::InvokeNativeTarget(execution, target->function, {})
                 : runtime::InvokeNativeEntry(execution, target->function, arguments);
    if (!result && result.error() != ExecutionStatus::ExecutionStopped) {
      last_diagnostic_ =
          execution.HasException()
              ? execution.ExceptionDiagnostic()
              : execution.Diagnostic(result.error(), ExecutionStatusName(result.error()));
    }
  }
  if (result_.status == DevelopmentStatus::Stopping) {
    FinishReload();
  }
  if (entered_generation != generation()) {
    return std::unexpected(ExecutionStatus::ExecutionStopped);
  }
  return result;
}

std::expected<runtime::Value, ExecutionStatus> DevelopmentSession::Invoke(
    std::string_view module,
    std::string_view name,
    std::span<const runtime::Value> arguments) {
  // 命名入口的临时句柄必须在进入脚本前释放，不能让会话自身的解析借用阻止 Reload。
  DeclarationId declaration;
  {
    auto entry = isolate_->native_dispatch().FindEntry(module, name);
    if (!entry) {
      return std::unexpected(entry.error());
    }
    declaration = entry->declaration();
  }
  return InvokeDeclaration(declaration, arguments, Invocation::Exported);
}

std::expected<runtime::Value, ExecutionStatus> DevelopmentSession::RunEntry() {
  const auto value = InvokeEntry({.module = entry_, .name = options_.entry});
  if (value) {
    entry_value_ = *value;
    entry_root_.Attach(isolate_->heap(), &entry_value_, 1);
  }
  return value;
}

std::expected<runtime::Value, ExecutionStatus> DevelopmentSession::InvokeEntry(
    DevelopmentEntry requested) {
  const auto source = snapshot();
  const auto records = source->identities.records();
  const auto module_uri = PathText(Utf8Path(requested.module).lexically_normal());
  const auto& functions = source->program.functions;
  const auto entry = std::ranges::find_if(functions, [&](const ir::Function& function) {
    const auto identity = function.identity;
    if (identity.local != 0 || identity.declaration == DeclarationId{} ||
        function.parameter_count != 0) {
      return false;
    }
    const auto& declaration =
        records.subspan(static_cast<std::size_t>(identity.declaration.value)).front().key;
    return declaration.kind == DeclarationKind::Function && declaration.name == requested.name &&
           records.subspan(static_cast<std::size_t>(declaration.owner.value)).front().key.name ==
               module_uri;
  });
  if (entry == functions.end()) {
    return std::unexpected(ExecutionStatus::InvalidEntry);
  }
  return InvokeDeclaration(entry->identity.declaration, {}, Invocation::Entry);
}

}  // namespace vela::compiler
