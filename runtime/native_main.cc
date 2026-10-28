#include "runtime/native_main.h"

#include <cstdint>
#include <iostream>

#include "base/process_exit.h"
#include "runtime/isolate.h"
#include "runtime/native_abi.h"
#include "runtime/native_call.h"
#include "runtime/native_instance.h"
#include "runtime/process_entry.h"

namespace vela::runtime {

extern "C" int vela_native_v1_main(const NativeImage* image, std::uint32_t function) {
  Isolate isolate;
  auto instance = NativeInstance::Instantiate(isolate, *image);
  if (!instance) {
    std::cerr << "vela: " << instance.error().message << '\n';
    return kRuntimeErrorExitCode;
  }
  ProcessEntry process(isolate, std::cerr);
  NativeExecution execution(**instance);
  const auto result = InvokeNativeTarget(execution, function, {});
  if (!result) {
    WriteRuntimeDiagnostic(std::cerr, execution.HasException()
                                          ? execution.ExceptionDiagnostic()
                                          : execution.Diagnostic(result.error(), {}));
    return kRuntimeErrorExitCode;
  }
  return process.Finish(*result);
}

}  // namespace vela::runtime
