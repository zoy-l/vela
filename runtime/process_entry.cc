#include "runtime/process_entry.h"

#include <ostream>
#include <string_view>

#include "base/diagnostic.h"
#include "base/execution_status.h"
#include "base/process_exit.h"
#include "base/source_location.h"
#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/process_exit.h"
#include "runtime/value.h"

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
  const auto diagnostic = future.error_diagnostic();
  output << "vela: " << label;
  if (diagnostic && !diagnostic->message.empty()) {
    output << ": " << diagnostic->message;
  }
  output << '\n';
  if (diagnostic) {
    WriteTrace(output, *diagnostic);
  }
}

}  // namespace

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

ProcessEntry::ProcessEntry(Isolate& isolate, std::ostream& errors)
    : isolate_(isolate), errors_(errors) {
  isolate_.scheduler().SetUnhandledErrorHandler([this](const Future& future, Value) {
    background_failed_ = true;
    WriteAsyncFailure(errors_, future, "unhandled Future error");
  });
}

ProcessEntry::~ProcessEntry() {
  isolate_.scheduler().SetUnhandledErrorHandler({});
}

int ProcessEntry::Finish(Value value) {
  auto& scheduler = isolate_.scheduler();
  const auto* future = value.IsObject() ? Heap::GetFuture(*value.AsObject()) : nullptr;
  auto root = future != nullptr ? future->Rooted() : Future::Value(value, &scheduler);
  root.AttachHeap(isolate_.heap());
  root.Observe([](FutureState, Value) {});
  if (!scheduler.PumpUntil(root, FuturePumpMode::WaitForHost)) {
    errors_ << "vela: async idle/deadlock: main has no pending work\n";
    return kRuntimeErrorExitCode;
  }
  while (scheduler.PumpOne(FuturePumpMode::WaitForHost)) {
  }
  if (root.state() == FutureState::Failed) {
    WriteAsyncFailure(errors_, root, "async main failed");
    return kRuntimeErrorExitCode;
  }
  if (background_failed_) {
    return kRuntimeErrorExitCode;
  }
  const auto exit_code = ProcessExitCode(root.value().value_or(Value::Null()));
  if (!exit_code) {
    errors_ << "vela: error: " << exit_code.error() << '\n';
    return kRuntimeErrorExitCode;
  }
  return *exit_code;
}

}  // namespace vela::runtime
