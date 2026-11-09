#pragma once

#include <cstdint>
#include <iosfwd>

#include "runtime/async/future.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class Isolate;
class NativeInstance;
struct TypeConstraint;

// 仅独立程序启动时调用；嵌入调用不改变宿主进程的信号处理。
void InitializeProcessOutput();
void WriteRuntimeDiagnostic(std::ostream& output, const FutureDiagnostic& diagnostic);
enum class ProcessStopDiagnostic : std::uint8_t { Report, Suppress };
[[nodiscard]] int RunProcessEntry(NativeInstance& instance,
                                  std::uint32_t function,
                                  std::ostream& errors,
                                  ProcessStopDiagnostic stop_diagnostic);

// 在脚本首段执行前安装错误观察；拥有本次进程入口的调度收尾与诊断。
class ProcessEntry final {
 public:
  ProcessEntry(Isolate& isolate, std::ostream& errors);
  ~ProcessEntry();
  ProcessEntry(const ProcessEntry&) = delete;
  ProcessEntry& operator=(const ProcessEntry&) = delete;
  ProcessEntry(ProcessEntry&&) = delete;
  ProcessEntry& operator=(ProcessEntry&&) = delete;

  // result_type 借用入口签名，nullptr 表示 void；Future 载荷使用规范运行时签名。
  [[nodiscard]] int Finish(Value value, const TypeConstraint* result_type);

 private:
  Isolate* isolate_;
  std::ostream* errors_;
  FutureRunScope run_;
  FutureScheduler::ErrorCallback previous_error_handler_;
  bool background_failed_ = false;
};

}  // namespace vela::runtime
