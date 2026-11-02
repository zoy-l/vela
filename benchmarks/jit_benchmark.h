#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "compiler/development/source_session.h"
#include "compiler/jit/code_resources.h"
#include "compiler/jit/compiler.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/value/value.h"

namespace benchmark {
class State;
}

namespace vela::benchmarks {

struct JitProgram final {
  std::string entry;
  compiler::jit::CompiledImage code;
  compiler::jit::CodeResourceCounts resources;
  compiler::SourceTimings frontend;
  std::chrono::nanoseconds capture{};
  std::chrono::nanoseconds engine{};
  std::chrono::nanoseconds source{};
  std::chrono::nanoseconds compilation{};
  std::chrono::nanoseconds preparation{};
  std::uint64_t compilation_peak_resident_bytes = 0;
};

// 注册项共享只读代码；每次校准和重复采样仍创建独立 Isolate、全局变量及 GC 状态。
class JitWorkload final {
 public:
  JitWorkload(std::string path, NativeCodeTier tier);
  [[nodiscard]] std::expected<std::shared_ptr<const JitProgram>, std::string> Prepare();

 private:
  std::string path_;
  NativeCodeTier tier_;
  std::optional<std::expected<std::shared_ptr<const JitProgram>, std::string>> program_;
};

void RegisterJitPreparation(std::string name, std::shared_ptr<JitWorkload> workload);
[[nodiscard]] std::expected<std::size_t, std::string> PrepareJitWorkloads(std::string filter);

struct JitSetup final {
  std::shared_ptr<const JitProgram> program;
  std::unique_ptr<runtime::Isolate> isolate;
  runtime::NativePublicationTimings publication;
  std::chrono::nanoseconds load{};
  std::chrono::nanoseconds pause{};
  std::string entry_name;
  std::optional<DeclarationId> entry;
};

[[nodiscard]] std::expected<JitSetup, std::string> LoadJitWorkload(
    JitWorkload& workload,
    runtime::NativeBindings bindings);
[[nodiscard]] std::expected<runtime::Value, ExecutionStatus> InvokeJitWorkload(
    JitSetup& setup,
    std::string_view name);
void ReportJitSetup(benchmark::State& state, const JitSetup& setup);
void ReportJitFirstExecution(benchmark::State& state,
                             std::chrono::steady_clock::time_point started);
void ReportJitMemory(benchmark::State& state, const JitSetup& setup);

}  // namespace vela::benchmarks
