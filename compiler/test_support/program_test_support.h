#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/parser.h"
#include "compiler/jit/source_compiler.h"
#include "runtime/async/future.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_instance.h"

namespace vela::compiler::test {

struct InstanceResult final {
  std::unique_ptr<runtime::NativeInstance> instance;
  frontend::Diagnostics diagnostics;
  [[nodiscard]] bool Succeeded() const noexcept { return instance != nullptr; }
};

// 编译器在返回前销毁；所有执行只使用原生实例保活的机器码和运行时元数据。
[[nodiscard]] InstanceResult CompileParsed(frontend::ParseResult parsed,
                                            runtime::Isolate& isolate,
                                            frontend::DiagnosticOptions options = {},
                                            jit::CompileOptions compilation = {});
[[nodiscard]] InstanceResult CompileSource(std::string_view source,
                                            runtime::Isolate& isolate,
                                            frontend::DiagnosticOptions options = {});
[[nodiscard]] InstanceResult CompileDebugSource(std::string_view source, runtime::Isolate& isolate);
[[nodiscard]] InstanceResult CompileModules(std::span<const frontend::SourceInput> sources,
                                             std::string_view entry,
                                             runtime::Isolate& isolate,
                                             frontend::DiagnosticOptions options = {});
[[nodiscard]] InstanceResult CompileFile(std::string_view path,
                                          runtime::Isolate& isolate,
                                          frontend::DiagnosticOptions options = {});
[[nodiscard]] std::expected<std::unique_ptr<runtime::NativeInstance>, std::string> CompileCleanSource(
    std::string_view source, runtime::Isolate& isolate);
[[nodiscard]] std::expected<std::unique_ptr<runtime::NativeInstance>, std::string> CompileCleanModules(
    std::span<const frontend::SourceInput> sources, std::string_view entry, runtime::Isolate& isolate);

// 原生映像在生产端 Isolate 销毁后实例化到另一堆；不保留 AST 或编译器。
[[nodiscard]] std::expected<std::unique_ptr<runtime::NativeInstance>, std::string> ReinstantiateSource(
    std::string_view source, runtime::Isolate& isolate);
[[nodiscard]] runtime::ExecutionResult Execute(runtime::NativeInstance& instance,
                                                 std::string_view entry = "main");
[[nodiscard]] runtime::Future StartFuture(runtime::NativeInstance& instance,
                                           std::string_view entry = "main");
[[nodiscard]] std::expected<std::int64_t, std::string> ExecuteInteger(runtime::NativeInstance& instance);
[[nodiscard]] std::expected<std::int64_t, std::string> AwaitInteger(runtime::Future& future,
                                                                    runtime::Isolate& isolate);
[[nodiscard]] std::expected<std::int64_t, std::string> ExecuteFutureInteger(
    runtime::NativeInstance& instance);
[[nodiscard]] std::expected<std::array<std::int64_t, 2>, std::string> ExecuteRepeated(
    runtime::NativeInstance& instance);
[[nodiscard]] std::expected<std::int64_t, std::string> RunInteger(std::string_view source,
                                                                  runtime::Isolate& isolate);

}  // namespace vela::compiler::test
