#include "runtime/native/native_main.h"

#include <cstdint>
#include <iostream>

#include "base/process_exit.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_instance.h"
#include "runtime/process/process_entry.h"

namespace vela::runtime {

extern "C" int vela_native_v1_main(const NativeImage* image, std::uint32_t function) {
  InitializeProcessOutput();
  Isolate isolate;
  auto instance = NativeInstance::Instantiate(isolate, *image);
  if (!instance) {
    std::cerr << "vela: " << instance.error().message << '\n';
    return kProgramFailureExitCode;
  }
  return RunProcessEntry(**instance, function, std::cerr, ProcessStopDiagnostic::Report);
}

}  // namespace vela::runtime
