#pragma once

#include <expected>
#include <functional>
#include <iosfwd>
#include <string_view>

namespace vela::runtime {

class Isolate;
class Value;
class Future;
struct FutureDiagnostic;

[[nodiscard]] std::expected<int, std::string_view> ProcessExitCode(Value value) noexcept;

void WriteRuntimeDiagnostic(std::ostream& output, const FutureDiagnostic& diagnostic);

// 在脚本首段执行前安装错误观察；拥有本次进程入口的调度收尾与诊断。
class ProcessEntry final {
 public:
  using ErrorObserver = std::function<void(const Future&, Value)>;
  ProcessEntry(Isolate& isolate, std::ostream& errors, ErrorObserver observer = {});
  ~ProcessEntry();
  ProcessEntry(const ProcessEntry&) = delete;
  ProcessEntry& operator=(const ProcessEntry&) = delete;
  ProcessEntry(ProcessEntry&&) = delete;
  ProcessEntry& operator=(ProcessEntry&&) = delete;

  [[nodiscard]] int Finish(Value value);

 private:
  Isolate* isolate_;
  std::ostream* errors_;
  ErrorObserver observer_;
  bool background_failed_ = false;
};

}  // namespace vela::runtime
