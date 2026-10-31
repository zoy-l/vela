#include "compiler/checkpoint_publication.h"

#include <string_view>
#include <utility>

namespace vela::compiler::test {

void CheckpointPublication::AtSafepoint(const runtime::NativeDebugContext& context) noexcept {
  constexpr std::string_view kCheckpointFunction = "checkpoint";
  if (context.top_frame().name() != kCheckpointFunction) {
    return;
  }
  PublishPending();
}

void CheckpointPublication::PublishPending() {
  if (auto candidate = std::exchange(pending, std::nullopt)) {
    (void)session_.Submit(std::move(*candidate));
    result = session_.Pump();
  }
}

}  // namespace vela::compiler::test
