#include "runtime/native_function.h"

#include <cstddef>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

#include "base/utf8.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

bool MatchesType(Value value, NativeType type) {
  switch (type) {
    case NativeType::Void:
      return value.IsNull();
    case NativeType::Boolean:
      return value.IsBoolean();
    case NativeType::Integer:
      return value.IsInteger();
    case NativeType::Double:
      return value.IsDouble();
    case NativeType::String:
      return value.IsString() && value.AsString() != nullptr;
    case NativeType::NullableString:
      return value.IsNull() || (value.IsString() && value.AsString() != nullptr);
  }
  std::unreachable();
}

}  // namespace

NativeResult NativeContext::String(std::string_view text) const {
  if (InvalidUtf8Offset(text)) {
    return std::unexpected(NativeError{
        .kind = NativeErrorKind::InvalidUtf8,
        .message = "native function returned invalid UTF-8",
    });
  }
  auto* string = isolate_->heap().AllocateString(text);
  if (string == nullptr) {
    return std::unexpected(NativeError{
        .kind = NativeErrorKind::OutOfMemory,
        .message = "out of memory creating native string",
    });
  }
  return Value::String(string);
}

std::string_view internal::NativeCodec<std::string_view>::Decode(Value value) {
  return value.AsString()->value();
}

NativeResult NativeFunction::Invoke(Isolate& isolate, std::span<const Value> arguments) const {
  if (arguments.size() != parameters_.size()) {
    return std::unexpected(NativeError{
        .kind = NativeErrorKind::TypeMismatch,
        .message = "native argument count mismatch",
    });
  }
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (!MatchesType(arguments.subspan(index).front(), parameters_.at(index))) {
      return std::unexpected(NativeError{
          .kind = NativeErrorKind::TypeMismatch,
          .message = "native argument type mismatch",
      });
    }
  }
  // 边界也允许宿主直接调用；根一次覆盖完整参数窗口，不为每个参数重复注册。
  RootRange roots(isolate.heap(), arguments.data(), arguments.size());
  NativeContext context(isolate);
  auto result = callback_(context, arguments);
  if (!result && InvalidUtf8Offset(result.error().message)) {
    return std::unexpected(NativeError{
        .kind = NativeErrorKind::InvalidUtf8,
        .message = "native function returned an invalid UTF-8 error",
    });
  }
  return result;
}

std::string_view NativeTypeName(NativeType type) noexcept {
  switch (type) {
    case NativeType::Void:
      return "void";
    case NativeType::Boolean:
      return "bool";
    case NativeType::Integer:
      return "int";
    case NativeType::Double:
      return "double";
    case NativeType::String:
      return "string";
    case NativeType::NullableString:
      return "string?";
  }
  std::unreachable();
}

}  // namespace vela::runtime
