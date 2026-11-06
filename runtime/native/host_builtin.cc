#include "runtime/native/host_builtin.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

#include <vela/error.h>
#include <vela/handles.h>
#include <vela/types.h>

#include "base/builtin.h"
#include "base/byte_access.h"
#include "base/execution_status.h"
#include "base/regexp.h"
#include "runtime/collections/byte_operations.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/library/builtins.h"
#include "runtime/native/host_value.h"
#include "runtime/native/native_type.h"
#include "runtime/native/native_type_traits.h"
#include "runtime/regexp/regexp.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::internal {
namespace {

std::expected<runtime::TypeConstraint, ExecutionStatus> OutputArrayType(runtime::Isolate& isolate,
                                                                        Builtin builtin) {
  runtime::NativeType type;
  switch (builtin) {
    case Builtin::StringCodePoints:
      type = runtime::internal::NativeTypeOf<Array<std::int64_t>>();
      break;
    case Builtin::RegExpAllMatches:
      type = runtime::internal::NativeTypeOf<Array<RegExpMatch>>();
      break;
    case Builtin::RegExpMatchGroups:
      type = runtime::internal::NativeTypeOf<Array<Nullable<String>>>();
      break;
    default:
      type = runtime::internal::NativeTypeOf<Array<String>>();
      break;
  }
  return runtime::ResolveBuiltinNativeType(isolate, type);
}

Result<void> UnicodeArray(HandleScope& scope,
                          Builtin builtin,
                          runtime::Value text,
                          runtime::Value& output) {
  auto& isolate = scope.isolate();
  auto& heap = isolate.heap();
  const auto type = OutputArrayType(isolate, builtin);
  if (!type) {
    return std::unexpected(Error::FromStatus(type.error()));
  }
  const runtime::RootRange type_root(heap, std::span(&*type, 1));
  const auto length = runtime::ExecuteBuiltin(
      builtin == Builtin::StringCodePoints ? Builtin::StringLength : Builtin::StringGraphemeLength,
      std::span(&text, 1), heap);
  if (!length) {
    return std::unexpected(Error::FromStatus(length.error()));
  }
  auto* array = heap.AllocateArray(*type->array, static_cast<std::size_t>(length->AsInteger()));
  if (array == nullptr) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::OutOfMemory));
  }
  // 字素填充会分配字符串；先把目标数组交给调用者已登记的结果根。
  StoreHostResult(heap, output, runtime::Value::Object(array));
  const auto filled = runtime::ExecuteBuiltin(builtin, std::array{text, output}, heap);
  if (!filled) {
    return std::unexpected(Error::FromStatus(filled.error()));
  }
  return {};
}

}  // namespace

Result<void> ExecuteHostBuiltin(HandleScope& scope,
                                Builtin builtin,
                                std::span<const runtime::Value> arguments,
                                runtime::Value& output) {
  auto& isolate = scope.isolate();
  auto& heap = isolate.heap();
  if (builtin == Builtin::StringCodePoints || builtin == Builtin::StringGraphemes) {
    return UnicodeArray(scope, builtin, arguments.front(), output);
  }
  if (IsRegExpBuiltin(builtin)) {
    const runtime::ArrayLayout* layout = nullptr;
    runtime::TypeConstraint contract;
    std::optional<runtime::RootRange> type_root;
    if (HasRegExpArrayLayout(builtin)) {
      const auto type = OutputArrayType(isolate, builtin);
      if (!type) {
        return std::unexpected(Error::FromStatus(type.error()));
      }
      contract = *type;
      type_root.emplace(heap, std::span(&contract, 1));
      layout = contract.array;
    }
    const auto result = runtime::ExecuteRegExpBuiltin(builtin, arguments, heap, layout);
    if (!result) {
      return std::unexpected(Error{
          .status = result.error().status,
          .message = result.error().message,
          .stack_trace = {},
      });
    }
    StoreHostResult(heap, output, *result);
    return {};
  }
  const auto result = IsByteBuiltin(builtin) ? runtime::ExecuteByteBuiltin(builtin, arguments, heap)
                                             : runtime::ExecuteBuiltin(builtin, arguments, heap);
  if (!result) {
    return std::unexpected(Error::FromStatus(result.error()));
  }
  StoreHostResult(heap, output, *result);
  return {};
}

}  // namespace vela::internal
