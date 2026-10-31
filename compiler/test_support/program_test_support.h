#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include "compiler/frontend/modules/source_loader.h"
#include "runtime/future.h"
#include "runtime/isolate.h"
#include "vm/program.h"

namespace vela::compiler::test {

// 正向测试不接受诊断；返回的 Program 独立持有执行所需的编译产物。
[[nodiscard]] std::expected<vm::Program, std::string> CompileCleanSource(std::string_view source,
                                                                         runtime::Isolate& isolate);
[[nodiscard]] std::expected<vm::Program, std::string> CompileCleanModules(
    std::span<const frontend::SourceInput> sources,
    std::string_view entry,
    runtime::Isolate& isolate);

// 入口句柄拥有程序；调用方须保持它存活，直到对象结果和异步执行检查结束。
[[nodiscard]] std::expected<vm::FunctionHandle, std::string> CompileEntry(
    std::string_view source,
    runtime::Isolate& isolate);

// 先销毁生产端 Isolate，再加载到调用方 Isolate；执行前销毁输入字节流并触发 GC。
[[nodiscard]] std::expected<vm::Program, std::string> ReloadSource(std::string_view source,
                                                                   runtime::Isolate& isolate);
[[nodiscard]] std::expected<std::int64_t, std::string> ExecuteInteger(const vm::Program& program,
                                                                      runtime::Isolate& isolate);
[[nodiscard]] std::expected<std::int64_t, std::string> AwaitInteger(runtime::Future& future,
                                                                    runtime::Isolate& isolate);
[[nodiscard]] std::expected<std::int64_t, std::string> ExecuteFutureInteger(
    const vm::Program& program,
    runtime::Isolate& isolate);
// 同一 VM 先执行程序副本，GC 后执行原程序；两份程序句柄保留到执行结束。
[[nodiscard]] std::expected<std::array<std::int64_t, 2>, std::string> ExecuteProgramCopy(
    const vm::Program& program,
    runtime::Isolate& isolate);
// 结果为独立标量，编译产物在返回前销毁；需检查存活程序的测试直接使用 ExecuteInteger。
[[nodiscard]] std::expected<std::int64_t, std::string> RunInteger(std::string_view source,
                                                                  runtime::Isolate& isolate);

}  // namespace vela::compiler::test
