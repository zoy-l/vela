#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/mock_operation.h"
#include "compiler/testing/report.h"
#include "compiler/testing/result.h"
#include "compiler/testing/settings.h"
#include "runtime/execution_context.h"
#include "runtime/future.h"
#include "runtime/isolate.h"
#include "runtime/process_entry.h"
#include "runtime/rooted_value.h"
#include "runtime/test_bridge.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::testing {

struct SuiteOptions final {
  Settings settings;
  // 过滤消费已注册的完整名字与继承标签，不能绕过编译和注册。
  std::function<std::expected<bool, std::string>(const TestResult&)> select;
  std::optional<runtime::FutureScheduler::Clock::time_point> load_deadline;
  bool list = false;
  std::uint64_t session = 1;
  std::uint64_t suite = 1;
  std::string file;
  ReportSink emit;
  std::function<bool()> is_cancelled;
};

// Program 和 Isolate 由执行线程拥有，生命周期覆盖 Suite；报告只保留已复制的数据。
class Suite final : public runtime::TestBridge {
 public:
  Suite(runtime::Isolate& isolate,
        const vm::Program& program,
        SuiteOptions options,
        std::ostream& diagnostics);
  ~Suite() override;
  Suite(const Suite&) = delete;
  Suite& operator=(const Suite&) = delete;
  Suite(Suite&&) = delete;
  Suite& operator=(Suite&&) = delete;
  [[nodiscard]] SuiteResult Run();
  [[nodiscard]] std::optional<std::uint64_t> CurrentTestId() const;
  std::expected<void, std::string> Register(const runtime::TestRegistration& registration) override;
  void RegisterMockFailure(const runtime::NominalType& type, MockFailureKind kind) override;
  std::expected<runtime::Value, std::string> Control(
      runtime::TestControl operation,
      std::span<const runtime::Value> arguments,
      std::vector<runtime::FutureTraceFrame> trace) override;

 private:
  enum class NodeKind : std::uint8_t { Group, Test };
  enum class State : std::uint8_t { Registering, Running, Finished };
  enum class WaitStatus : std::uint8_t { Completed, TimedOut, Idle, Cancelled };
  enum class ExpectationKind : std::uint8_t { Assertion, CallObservation };
  struct Callback final {
    std::uint64_t id;
    runtime::RootedValue value;
    std::vector<runtime::FutureTraceFrame> trace;
  };
  struct AsyncExpectation final {
    std::optional<runtime::Future> future;
    std::vector<runtime::FutureTraceFrame> trace;
    bool waiting = true;
    std::shared_ptr<runtime::HostEventSource> producer;
  };
  struct Node final {
    std::uint64_t id = 0;
    NodeKind kind = NodeKind::Group;
    Node* parent = nullptr;
    std::string description;
    std::vector<std::string> tags;
    std::optional<Skip> skip;
    std::optional<Timeout> timeout;
    std::optional<Callback> body;
    std::array<std::vector<Callback>, 4> hooks;
    std::vector<Callback> cleanup;
    std::vector<Callback> finalizers;
    std::vector<std::unique_ptr<Node>> children;
    std::vector<runtime::FutureTraceFrame> trace;
    std::optional<std::size_t> result;
    std::optional<std::chrono::milliseconds> effective_timeout;
    std::size_t runnable = 0;
    bool closed = false;
    bool cancelled = false;
    runtime::FutureScheduler::Clock::time_point heartbeat;
    std::size_t pending_expectations = 0;
    std::size_t pending_callbacks = 0;
    std::vector<AsyncExpectation> expectations;
    std::vector<std::uint64_t> callback_expectations;
    std::vector<TestError> errors;
  };
  struct CallbackExpectation final {
    enum class State : std::uint8_t { Pending, Satisfied, Abandoned };
    runtime::ExecutionContext context;
    std::int64_t minimum = 1;
    std::int64_t maximum = 1;
    std::int64_t calls = 0;
    State state = State::Pending;
    std::vector<runtime::FutureTraceFrame> trace;
  };
  [[nodiscard]] Callback Capture(const runtime::TestRegistration& registration);
  std::expected<void, std::string> RegisterNode(const runtime::TestRegistration& registration);
  std::expected<void, std::string> RegisterHook(const runtime::TestRegistration& registration);
  static std::expected<void, std::string> ReadMetadata(
      Node& node,
      const runtime::TestRegistration& registration);
  std::expected<void, std::string> Select(Node& node,
                                          const std::string& prefix,
                                          const std::vector<std::string>& inherited_tags);
  std::expected<void, std::string> Configure(Node& node, TestResult& result);
  [[nodiscard]] std::expected<bool, std::string> IsSelected(const TestResult& result) const;
  [[nodiscard]] static bool IsCleanupPhase(TestPhase phase);
  void RunGroup(Node& group, std::vector<Node*>& ancestry, const std::vector<TestError>& blocked);
  void RunTest(Node& test,
               const std::vector<Node*>& ancestry,
               const std::vector<TestError>& blocked);
  void RunHooks(Node& group,
                std::size_t index,
                TestPhase phase,
                Node& target,
                std::vector<TestError>& errors);
  void RunCleanup(Node& scope, std::vector<TestError>& errors);
  void Invoke(const Callback& callback,
              TestPhase phase,
              Node& scope,
              std::vector<TestError>& errors);
  [[nodiscard]] vm::ExecutionResult Await(vm::ExecutionResult result,
                                          std::optional<std::chrono::milliseconds> timeout,
                                          Node& scope,
                                          TestPhase phase);
  [[nodiscard]] WaitStatus WaitUntil(Node& scope,
                                     std::optional<std::chrono::milliseconds> timeout,
                                     TestPhase phase,
                                     const std::function<bool()>& pending);
  void WaitExpectations(Node& scope, TestPhase phase, std::vector<TestError>& errors);
  void AbandonExpectations(Node& scope);
  [[nodiscard]] vm::ExecutionResult WaitFailure(WaitStatus status) const;
  [[nodiscard]] std::expected<runtime::ExecutionContext, std::string> ExecutingContext();
  std::expected<runtime::Value, std::string> TrackExpectation(
      runtime::Value value,
      std::vector<runtime::FutureTraceFrame> trace,
      ExpectationKind kind);
  std::expected<runtime::Value, std::string> RegisterCallback(
      std::span<const runtime::Value> arguments,
      std::vector<runtime::FutureTraceFrame> trace);
  std::expected<runtime::Value, std::string> EnterCallback(runtime::Value identity);
  std::expected<runtime::Value, std::string> ExitCallback(runtime::Value ticket);
  std::expected<runtime::Value, std::string> CheckCallback(runtime::Value identity);
  [[nodiscard]] runtime::Value CurrentTestScope() const;
  std::expected<runtime::Value, std::string> RegisterFinalizer(
      runtime::Value callback,
      std::vector<runtime::FutureTraceFrame> trace);
  std::expected<runtime::Value, std::string> ReleaseFinalizer(runtime::Value identity);
  void DeliverError(const runtime::FutureResult& completion,
                    std::uint64_t identity,
                    TestPhase phase);
  [[nodiscard]] TestError Describe(const vm::ExecutionResult& result, TestPhase phase) const;
  void DescribeMockFailure(runtime::Value value, TestError& error) const;
  void Unhandled(const runtime::Future& future, runtime::Value value);
  void RecordError(const vm::ExecutionResult& failure,
                   TestPhase phase,
                   std::vector<TestError>& errors);

  void Emit(ReportEvent event) const;
  void EmitTest(const Node& node, ReportEventType type) const;
  SuiteResult Finish();

  runtime::Isolate* isolate_;
  const vm::Program* program_;
  SuiteOptions options_;
  vm::VirtualMachine machine_;
  Node root_;
  Node* registration_scope_ = &root_;
  State state_ = State::Registering;
  std::uint64_t next_id_ = 1;
  std::uint64_t next_invocation_ = 1;
  absl::flat_hash_map<std::uint64_t, Node*> scopes_;
  absl::flat_hash_map<std::uint64_t, TestPhase> phases_;
  absl::flat_hash_map<std::uint64_t, std::uint64_t> hook_invocations_;
  absl::flat_hash_set<std::uint64_t> reported_errors_;
  absl::flat_hash_set<std::string> declared_tags_;
  absl::flat_hash_set<std::string> warned_tags_;
  absl::flat_hash_map<std::uint64_t, CallbackExpectation> callbacks_;
  absl::flat_hash_map<const runtime::NominalType*, MockFailureKind> mock_failures_;
  std::vector<std::optional<runtime::ExecutionContext>> callback_contexts_;
  SuiteResult result_;
  runtime::ProcessEntry process_;
};

}  // namespace vela::testing
