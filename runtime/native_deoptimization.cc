#include "runtime/native_deoptimization.h"

#include <cstdint>

#include "runtime/isolate.h"
#include "runtime/native_abi.h"
#include "runtime/native_dispatch.h"
#include "runtime/native_image.h"

namespace vela::runtime {

bool CanUseNativeOptimization(const NativeExecution& execution, const NativeImage& image) noexcept {
  return execution.isolate().native_dispatch().revision() == image.revision;
}

extern "C" std::uint8_t vela_native_v4_can_optimize(const NativeExecution* execution,
                                                    const NativeImage* image) noexcept {
  return execution != nullptr && CanUseNativeOptimization(*execution, *image) ? 1 : 0;
}

extern "C" std::uint8_t vela_native_v4_recover(NativeExecution* execution,
                                               NativeCheckpoint* checkpoint,
                                               void* result) noexcept {
  return execution->Recover(*checkpoint, result);
}

}  // namespace vela::runtime
