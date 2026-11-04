#include "compiler/bindgen/test_host.h"

#include <cstdint>
#include <expected>
#include <memory>

#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_context.h"

namespace vela::binding_fixture {

std::int64_t Counter::Collect(runtime::NativeContext& context, std::int64_t delta) noexcept {
  context.isolate().heap().Collect();
  return Adjust(delta);
}

std::shared_ptr<Counter> Counter::Shared(std::int64_t initial) noexcept {
  return std::make_shared<Counter>(initial);
}

std::expected<std::int64_t, runtime::NativeError> Counter::Checked(bool fail) const noexcept {
  if (fail) {
    return std::unexpected(runtime::NativeError{
        .kind = runtime::NativeErrorKind::Failure,
        .message = "fixture failure",
    });
  }
  return value_;
}

}  // namespace vela::binding_fixture
