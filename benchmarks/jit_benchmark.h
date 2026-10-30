#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include "base/execution_status.h"
#include "compiler/development_session.h"
#include "compiler/jit/compiler.h"
#include "runtime/isolate.h"
#include "runtime/value.h"

namespace benchmark {
class State;
}

namespace vela::benchmarks {

struct JitSetup final {
  std::unique_ptr<compiler::DevelopmentSession> session;
  std::chrono::nanoseconds capture{};
  std::chrono::nanoseconds engine{};
  std::chrono::nanoseconds load{};
  std::uint64_t compilation_peak_resident_bytes = 0;
};

[[nodiscard]] std::expected<JitSetup, std::string> LoadJitWorkload(
    const std::string& path,
    runtime::NativeBindings bindings,
    compiler::jit::CompileOptions options);
[[nodiscard]] std::expected<runtime::Value, ExecutionStatus> InvokeJitWorkload(
    compiler::DevelopmentSession& session,
    std::string_view name);
void ReportJitSetup(benchmark::State& state, const JitSetup& setup);
void ReportJitMemory(benchmark::State& state, compiler::DevelopmentSession& session);

}  // namespace vela::benchmarks
