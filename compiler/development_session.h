#pragma once

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
#include <vector>

#include "base/declaration.h"
#include "base/execution_status.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/code_cache.h"
#include "compiler/jit/code_resources.h"
#include "compiler/jit/compiler.h"
#include "compiler/source_session.h"
#include "runtime/external_resources.h"
#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_control.h"
#include "runtime/native_debugger.h"
#include "runtime/native_dispatch.h"
#include "runtime/value.h"

namespace vela::compiler {

enum class DevelopmentOperation : std::uint8_t { Load, HotReload, Reload };
enum class DevelopmentStatus : std::uint8_t {
  Prepared,
  WaitingForSafepoint,
  WaitingForRelease,
  Stopping,
  Accepted,
  SourceError,
  CodeError,
  RequiresReload,
  InstantiationFailed,
  Stale,
  Cancelled,
  Busy,
  RefreshFailed,
  EntryFailed,
  InvalidCandidate,
};

struct DevelopmentTimings final {
  std::chrono::nanoseconds source{};
  std::chrono::nanoseconds code{};
  std::chrono::nanoseconds preparation{};
  std::chrono::nanoseconds waiting{};
  std::chrono::nanoseconds pause{};
  std::chrono::nanoseconds stopping{};
  std::chrono::nanoseconds refresh{};
  std::chrono::nanoseconds entry{};
  jit::CompileTimings native;
  runtime::NativePublicationTimings publication;
  SourceTimings frontend;
};

struct DevelopmentChanges final {
  std::vector<std::string> modules;
  std::size_t added_declarations = 0;
  std::size_t removed_declarations = 0;
  std::size_t compiled_functions = 0;
};

struct DevelopmentResult final {
  DevelopmentStatus status = DevelopmentStatus::Prepared;
  DevelopmentOperation operation = DevelopmentOperation::Load;
  std::uint64_t request = 0;
  std::uint64_t generation = 1;
  std::uint64_t revision = 0;
  std::string message;
  frontend::Diagnostics diagnostics;
  DeclarationId declaration;
  std::optional<FunctionIdentity> dependency = std::nullopt;
  std::vector<runtime::ExternalResourceInfo> blockers;
  std::optional<ExecutionStatus> execution = std::nullopt;
  DevelopmentChanges changes;
  DevelopmentTimings timings;
  jit::CodeCacheStatus cache = jit::CodeCacheStatus::Disabled;
};

struct DevelopmentSessionIdentity final {};
class DevelopmentSession;

struct DevelopmentEntry final {
  std::string_view module;
  std::string_view name;
};

class DevelopmentCandidate final {
 public:
  [[nodiscard]] const DevelopmentResult& result() const noexcept { return result_; }

 private:
  friend class DevelopmentSession;
  DevelopmentCandidate(std::shared_ptr<const DevelopmentSessionIdentity> session,
                       SourceUpdate update,
                       SourceCandidate source,
                       jit::CompiledImage code,
                       std::unique_ptr<SourceSession> replacement,
                       DevelopmentResult result);
  std::shared_ptr<const DevelopmentSessionIdentity> session_;
  SourceUpdate update_;
  SourceCandidate source_;
  jit::CompiledImage code_;
  std::unique_ptr<SourceSession> replacement_;
  DevelopmentResult result_;
};

struct DevelopmentOptions final {
  std::size_t gc_threshold = runtime::kDefaultGcThreshold;
  std::string entry = "main";
  jit::CompileOptions compilation;
  // 借用调试器；它须覆盖会话，或在销毁前通过 SetDebugger 解除注册。
  runtime::NativeDebugger* debugger = nullptr;
  // 所属线程上的阶段通知只消费结果；观察者不得执行脚本或重入会话操作。
  std::function<void(const DevelopmentResult&)> progress;
  // 只在提交完成后通知；回调通过会话命名入口调用当前代码，并用 expected 报告刷新错误。
  std::function<std::expected<void, ExecutionStatus>(DevelopmentSession&, const DevelopmentResult&)>
      refreshed;
};

// 会话独占源码提交与 Isolate 切换。宿主在所属线程操作，外部根仍须先释放才能 Reload。
// Prepare 可在隔离工作线程编译，不访问 Isolate；其线程须在会话销毁前退出。
// 其他操作在所属线程执行。Submit 的提交动作由精确安全点消费，Invoke/Pump 完成停止收尾。
class DevelopmentSession final {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<DevelopmentSession>, frontend::Diagnostics>
  Create(std::string entry, runtime::NativeBindings bindings = {}, DevelopmentOptions options = {});
  ~DevelopmentSession();
  DevelopmentSession(const DevelopmentSession&) = delete;
  DevelopmentSession& operator=(const DevelopmentSession&) = delete;
  DevelopmentSession(DevelopmentSession&&) = delete;
  DevelopmentSession& operator=(DevelopmentSession&&) = delete;

  [[nodiscard]] std::expected<DevelopmentCandidate, DevelopmentResult> Prepare(
      SourceUpdate update,
      DevelopmentOperation operation);
  [[nodiscard]] DevelopmentResult Submit(DevelopmentCandidate candidate);
  [[nodiscard]] std::expected<void, runtime::NativeSafepointError> Post(
      DevelopmentCandidate candidate,
      std::function<void(const DevelopmentResult&)> submitted = {});
  [[nodiscard]] runtime::NativeSafepointEndpoint safepoint_endpoint() const;
  void SetDebugger(runtime::NativeDebugger* debugger) noexcept;
  [[nodiscard]] DevelopmentResult Pump();
  [[nodiscard]] std::expected<void, DevelopmentStatus> Cancel();
  [[nodiscard]] std::expected<runtime::Value, ExecutionStatus> Invoke(
      std::string_view module,
      std::string_view name,
      std::span<const runtime::Value> arguments = {});
  [[nodiscard]] std::expected<runtime::Value, ExecutionStatus> RunEntry();
  // 受信任宿主/REPL 可明确执行未导出的零参数入口，不改变主入口的 Future 根。
  [[nodiscard]] std::expected<runtime::Value, ExecutionStatus> InvokeEntry(
      DevelopmentEntry requested);
  [[nodiscard]] runtime::Isolate& isolate() noexcept { return *isolate_; }
  [[nodiscard]] std::shared_ptr<const CompiledSourceSnapshot> snapshot() const noexcept;
  [[nodiscard]] std::uint64_t generation() const noexcept;
  [[nodiscard]] const DevelopmentResult& result() const noexcept { return result_; }
  [[nodiscard]] jit::CodeResourceCounts code_resources() const noexcept;
  [[nodiscard]] runtime::Value entry_value() const noexcept { return entry_value_; }
  [[nodiscard]] const std::optional<runtime::FutureDiagnostic>& last_diagnostic() const noexcept {
    return last_diagnostic_;
  }

 private:
  enum class Invocation : std::uint8_t { Exported, Entry };
  struct CompilationBase final {
    std::shared_ptr<SourceSession> sources;
    std::uint64_t generation = 0;
    std::uint64_t request = 0;
  };
  struct Pending final {
    explicit Pending(DevelopmentCandidate source);
    DevelopmentCandidate candidate;
    std::unique_ptr<runtime::Isolate> replacement;
    std::chrono::steady_clock::time_point queued;
    std::chrono::steady_clock::time_point stopping;
  };
  DevelopmentSession(std::string entry,
                     runtime::NativeBindings bindings,
                     DevelopmentOptions options,
                     std::unique_ptr<jit::Compiler> compiler);
  void AtSafepoint();
  void Publish();
  void BeginReload();
  void FinishReload();
  void Notify();
  void Report(const DevelopmentResult& result) const;
  std::expected<runtime::Value, ExecutionStatus> InvokeDeclaration(
      DeclarationId declaration,
      std::span<const runtime::Value> arguments,
      Invocation invocation);
  [[nodiscard]] std::vector<runtime::ExternalResourceInfo> HostBlockers() const;
  [[nodiscard]] CompilationBase BeginCompilation();
  std::expected<DevelopmentCandidate, DevelopmentResult> Prepare(
      SourceUpdate update,
      DevelopmentOperation operation,
      std::span<const DeclarationId> retained);

  const std::shared_ptr<const DevelopmentSessionIdentity> identity_;
  std::string entry_;
  runtime::NativeBindings bindings_;
  DevelopmentOptions options_;
  std::unique_ptr<jit::Compiler> compiler_;
  std::shared_ptr<SourceSession> sources_;
  std::unique_ptr<runtime::Isolate> isolate_;
  runtime::NativeSafepointEndpoint endpoint_;
  std::unique_ptr<Pending> pending_;
  DevelopmentResult result_;
  runtime::Value entry_value_;
  std::optional<runtime::FutureDiagnostic> last_diagnostic_;
  runtime::RootRange entry_root_{runtime::RootOwner::Runtime};
  mutable std::mutex source_mutex_;
  std::uint64_t generation_ = 1;
  std::uint64_t next_request_ = 1;
};

}  // namespace vela::compiler
