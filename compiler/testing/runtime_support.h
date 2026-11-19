#pragma once

#include <cstdint>

namespace vela::runtime {

class NativeExecution;
struct NativeValue;

}  // namespace vela::runtime

namespace vela::testing {

// 开发执行引擎安装此入口；生产运行库不提供测试内建调用的实现。
extern "C" std::uint8_t vela_development_v1_builtin(runtime::NativeExecution* execution,
                                                    std::uint32_t operation,
                                                    const runtime::NativeValue* arguments,
                                                    runtime::NativeValue* result);

}  // namespace vela::testing
