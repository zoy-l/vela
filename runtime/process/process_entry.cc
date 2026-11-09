#include "runtime/process/process_entry.h"

#include <cstdint>
#include <functional>
#include <ostream>
#include <string_view>
#include <utility>

#ifndef _WIN32
#include <csignal>
#endif

#include "base/diagnostic.h"
#include "base/execution_status.h"
#include "base/process_exit.h"
#include "base/source_location.h"
#include "runtime/async/future.h"
#include "runtime/async/future_operations.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_control.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_io.h"

namespace vela::runtime {
namespace {

void WriteTrace(std::ostream& output, const FutureDiagnostic& diagnostic) {
  for (const auto& frame : diagnostic.stack_trace) {
    output << "  at " << (frame.function.empty() ? "<anonymous>" : frame.function);
    if (!frame.source_path.empty()) {
      output << " (" << frame.source_path << ':' << frame.line << ':' << frame.column << ')';
    } else {
      output << " (instruction " << frame.instruction << ')';
    }
    output << '\n';
  }
}

void WriteAsyncFailure(std::ostream& output, const Future& future, std::string_view label) {
  const auto& diagnostic = future.result().diagnostic;
  output << "vela: " << label;
  if (!diagnostic.message.empty()) {
    output << ": " << diagnostic.message;
  }
  output << '\n';
  WriteTrace(output, diagnostic);
}

}  // namespace

void InitializeProcessOutput() {
#ifndef _WIN32
  // 让关闭的输出管道成为可报告的 I/O 失败，而非先由默认信号动作终止进程。
  std::signal(SIGPIPE, SIG_IGN);
#endif
}

void WriteRuntimeDiagnostic(std::ostream& output, const FutureDiagnostic& diagnostic) {
  const auto message = diagnostic.message.empty() ? ExecutionStatusName(diagnostic.status)
                                                  : std::string_view(diagnostic.message);
  if (diagnostic.stack_trace.empty()) {
    WriteError(output, {}, SourceLocation(), message);
  } else {
    const auto& origin = diagnostic.stack_trace.front();
    WriteError(output, origin.source_path, {.line = origin.line, .column = origin.column}, message);
  }
  WriteTrace(output, diagnostic);
}

int RunProcessEntry(NativeInstance& instance,
                    std::uint32_t function,
                    std::ostream& errors,
                    ProcessStopDiagnostic stop_diagnostic) {
  ProcessEntry process(instance.isolate(), errors);
  const auto result = ExecuteNativeFunction(instance, function);
  if (!result.Succeeded()) {
    if (result.status != ExecutionStatus::ExecutionStopped ||
        stop_diagnostic == ProcessStopDiagnostic::Report) {
      WriteRuntimeDiagnostic(errors, {
                                         .message = result.message,
                                         .stack_trace = result.stack_trace,
                                         .status = result.status,
                                         .origin = result.origin,
                                     });
    }
    return kProgramFailureExitCode;
  }
  const auto type = instance.call_signature(function).result;
  return process.Finish(result.value, type ? &instance.layouts().types[*type] : nullptr);
}

ProcessEntry::ProcessEntry(Isolate& isolate, std::ostream& errors)
    : isolate_(&isolate), errors_(&errors), run_(isolate.scheduler()) {
  previous_error_handler_ =
      isolate_->scheduler().SetUnhandledErrorHandler([this](const Future& future, Value error) {
        if (!run_.IsOwnerOf(future)) {
          if (previous_error_handler_) {
            previous_error_handler_(future, error);
          }
          return;
        }
        background_failed_ = true;
        WriteAsyncFailure(*errors_, future, "unhandled Future error");
      });
}

ProcessEntry::~ProcessEntry() {
  run_.Close();
  isolate_->scheduler().SetUnhandledErrorHandler(std::move(previous_error_handler_));
}

int ProcessEntry::Finish(Value value, const TypeConstraint* result_type) {
  auto& scheduler = isolate_->scheduler();
  auto root = RootFutureValue(*isolate_, value);
  root.MarkObserved();
  if (AsFuture(value) != nullptr) {
    const auto* signature = Heap::TypeOf(value).signature;
    result_type = signature->result ? &*signature->result : nullptr;
  }
  auto& control = isolate_->native_control();
  const std::function<bool()> pending = [&] {
    if (!root.is_pending()) {
      return false;
    }
    return control.ProcessSafepoint() != ExecutionStatus::ExecutionStopped && root.is_pending();
  };
  while (pending()) {
    static_cast<void>(scheduler.PumpOne(FuturePumpMode::WaitForCompletion, pending));
  }
  run_.Close();
  if (control.IsStopRequested()) {
    return 0;
  }
  if (root.state() == FutureState::Failed) {
    WriteAsyncFailure(*errors_, root, "async main failed");
    return kProgramFailureExitCode;
  }
  if (background_failed_) {
    return kProgramFailureExitCode;
  }
  auto& output = isolate_->output();
  if (result_type != nullptr) {
    WriteValue(output, root.result().value);
    output << '\n';
  }
  output.flush();
  if (!output) {
    *errors_ << "vela: error: failed to write program output\n";
    return kProgramFailureExitCode;
  }
  return 0;
}

}  // namespace vela::runtime
