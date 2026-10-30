#pragma once

#include <cstdint>
#include <optional>

namespace vela::runtime {

class FutureScheduler;

// 只保存会话内身份，不拥有 Program、GC 对象或宿主报告记录。
struct ExecutionContext final {
  std::uint64_t session = 0;
  std::uint64_t suite = 0;
  std::uint64_t scope = 0;
  std::uint64_t invocation = 0;
  bool operator==(const ExecutionContext&) const = default;
};

struct ErrorOrigin final {
  std::optional<ExecutionContext> context;
  std::uint64_t identity = 0;
  // 没有测试上下文也是已经确定的来源；转发不能把它重新归属到当前测试。
  bool captured = false;
};

class ExecutionContextScope final {
 public:
  ExecutionContextScope(FutureScheduler& scheduler, std::optional<ExecutionContext> context);
  ~ExecutionContextScope();
  ExecutionContextScope(const ExecutionContextScope&) = delete;
  ExecutionContextScope& operator=(const ExecutionContextScope&) = delete;
  ExecutionContextScope(ExecutionContextScope&&) = delete;
  ExecutionContextScope& operator=(ExecutionContextScope&&) = delete;

 private:
  FutureScheduler* scheduler_;
  std::optional<ExecutionContext> previous_;
};

}  // namespace vela::runtime
