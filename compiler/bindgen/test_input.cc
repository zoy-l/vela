#include "base/execution_status.h"
#include "compiler/bindgen/test_host.h"

#include <cstdint>
#include <expected>
#include <memory>

#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_context.h"
#include "vela/error.h"
#include "vela/handles.h"
#include "vela/types.h"

namespace vela::binding_fixture {

std::int64_t Counter::Collect(runtime::NativeContext& context, std::int64_t delta) noexcept {
  context.isolate().heap().Collect();
  return Adjust(delta);
}

std::shared_ptr<Counter> Counter::Shared(std::int64_t initial) noexcept {
  return std::make_shared<Counter>(initial);
}

Local<Array<std::int64_t>> Counter::Echo(runtime::NativeContext& context,
                                         Local<Array<std::int64_t>> values) noexcept {
  context.isolate().heap().Collect();
  return values;
}

std::expected<std::int64_t, vela::Error> Counter::Checked(bool fail) const noexcept {
  if (fail) {
    return std::unexpected(vela::Error{
        .status = vela::ExecutionStatus::NativeFailure,
        .message = "fixture failure",
        .stack_trace = {},
    });
  }
  return value_;
}

}  // namespace vela::binding_fixture
