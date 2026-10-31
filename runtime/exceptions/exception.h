#pragma once

#include <cstdint>
#include <string_view>

#include "base/execution_status.h"
#include "runtime/async/execution_context.h"
#include "runtime/value/object.h"

namespace vela::runtime {

struct FutureDiagnostic;

// 隐藏的异常包络不暴露给脚本。payload 是 catch 可见的强类型类实例。
// 此数据与消息紧随 HeapObject，均计入一次堆分配。
struct ExceptionData final {
  HeapObject* payload = nullptr;
  ExecutionStatus status = ExecutionStatus::Thrown;
  std::uint32_t message_length = 0;
  // 异常拥有已经解析的版本诊断，不依赖机器码或原生映像继续存活。
  FutureDiagnostic* diagnostic = nullptr;
  ErrorOrigin origin;

  [[nodiscard]] std::string_view message() const noexcept;
  [[nodiscard]] static ExceptionData& From(HeapObject& object) noexcept;
};

}  // namespace vela::runtime
