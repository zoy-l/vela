#include "compiler/test_support/checkpoint_publication.h"

#include <optional>
#include <string_view>
#include <utility>
#include "runtime/native_debugger.h"

namespace vela::compiler::test {

void CheckpointPublication::AtSafepoint(const runtime::NativeDebugContext& context) noexcept {
  constexpr std::string_view kCheckpointFunction = "checkpoint";
  if (context.top_frame().name() != kCheckpointFunction) {
    return;
  }
  PublishPending();
}

void CheckpointPublication::PublishPending() {
  if (auto candidate = std::exchange(pending_, std::nullopt)) {
    (void)session_.Submit(std::move(*candidate));
    result_ = session_.Pump();
  }
}

void CheckpointPublication::Stage(DevelopmentCandidate candidate) {
  pending_ = std::move(candidate);
}

}  // namespace vela::compiler::test
