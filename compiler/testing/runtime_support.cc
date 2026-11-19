#include "compiler/testing/runtime_support.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "compiler/testing/native_mock.h"
#include "compiler/testing/native_test.h"
#include "compiler/testing/test_inspection.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/value/value.h"
#include "runtime/value/value_operations.h"

namespace vela::testing {

extern "C" std::uint8_t vela_development_v1_builtin(runtime::NativeExecution* execution,
                                                    std::uint32_t operation,
                                                    const runtime::NativeValue* arguments,
                                                    runtime::NativeValue* result) {
  const auto builtin = static_cast<Builtin>(operation);
  const auto count = BuiltinArity(builtin);
  const auto input = std::span(arguments, count);
  std::array<runtime::Value, kMaximumBuiltinArity> values;
  const auto decoded = std::span(values).first(count);
  for (std::size_t index = 0; index < count; ++index) {
    decoded[index] = runtime::DecodeNativeValue(input[index]);
  }
  switch (builtin) {
    case Builtin::TestRegister:
    case Builtin::TestControl:
      return runtime::internal::StoreNativeResult(result,
                                                  ExecuteNativeTest(*execution, builtin, decoded));
    case Builtin::MockInspect:
      return runtime::internal::StoreNativeResult(result, ExecuteNativeMock(*execution, decoded));
    case Builtin::TestInspect:
      return runtime::internal::StoreNativeResult(
          result, InspectTestValue(decoded, execution->isolate().heap()));
    case Builtin::TestSame:
      *result = runtime::EncodeNativeValue(
          runtime::Value::Boolean(runtime::IsEqualValue(decoded.front(), decoded[1])));
      return runtime::ToNativeStatus(ExecutionStatus::Returned);
    default:
      std::unreachable();
  }
}

}  // namespace vela::testing
