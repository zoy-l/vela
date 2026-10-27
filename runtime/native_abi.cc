#include "runtime/native_abi.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>

#include "base/execution_status.h"
#include "runtime/isolate.h"
#include "runtime/native_function.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

constexpr std::uint8_t kReturned = static_cast<std::uint8_t>(ExecutionStatus::Returned);
constexpr std::uint8_t kTypeError = static_cast<std::uint8_t>(ExecutionStatus::TypeError);
constexpr std::uint8_t kIntegerOverflow =
    static_cast<std::uint8_t>(ExecutionStatus::IntegerOverflow);
constexpr std::uint8_t kDivisionByZero = static_cast<std::uint8_t>(ExecutionStatus::DivisionByZero);

bool IsKind(const NativeValue& value, NativeValueKind kind) noexcept {
  return value.kind == kind;
}

std::uint8_t Store(NativeValue* destination, NativeValueKind kind, std::uint64_t payload) noexcept {
  destination->payload = payload;
  destination->kind = kind;
  return kReturned;
}

std::uint8_t StoreInteger(NativeValue* destination, std::int64_t value) noexcept {
  return Store(destination, NativeValueKind::Integer, std::bit_cast<std::uint64_t>(value));
}

std::uint8_t StoreDouble(NativeValue* destination, double value) noexcept {
  return Store(destination, NativeValueKind::Double, std::bit_cast<std::uint64_t>(value));
}

std::int64_t ReadInteger(const NativeValue& value) noexcept {
  return std::bit_cast<std::int64_t>(value.payload);
}

double ReadDouble(const NativeValue& value) noexcept {
  return std::bit_cast<double>(value.payload);
}

enum class IntOperation : std::uint8_t {
  Add,
  Subtract,
  Multiply,
  Divide,
  Modulo,
  BitAnd,
  BitOr,
  BitXor,
  ShiftLeft,
  ShiftRight,
  ShiftUnsignedRight,
};

enum class UnaryOperation : std::uint8_t { Negate, BitNot, Not };

enum class CompareOperation : std::uint8_t {
  Equal,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
};

constexpr std::int64_t kIntegerBitCount = 64;

bool CheckedAdd(std::int64_t left, std::int64_t right, std::int64_t* result) noexcept {
  if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
    return false;
  }
  *result = left + right;
  return true;
}

bool CheckedSub(std::int64_t left, std::int64_t right, std::int64_t* result) noexcept {
  if ((right < 0 && left > std::numeric_limits<std::int64_t>::max() + right) ||
      (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right)) {
    return false;
  }
  *result = left - right;
  return true;
}

bool CheckedMul(std::int64_t left, std::int64_t right, std::int64_t* result) noexcept {
  if (left == 0 || right == 0) {
    *result = 0;
    return true;
  }
  if (left == -1 && right == std::numeric_limits<std::int64_t>::min()) {
    return false;
  }
  if (right == -1 && left == std::numeric_limits<std::int64_t>::min()) {
    return false;
  }
  if (left > std::numeric_limits<std::int64_t>::max() / right ||
      left < std::numeric_limits<std::int64_t>::min() / right) {
    return false;
  }
  *result = left * right;
  return true;
}

std::uint8_t ApplyIntegerOperation(IntOperation operation,
                                   std::int64_t left,
                                   std::int64_t right,
                                   std::int64_t* result) noexcept {
  switch (operation) {
    case IntOperation::Add:
      if (!CheckedAdd(left, right, result)) {
        return kIntegerOverflow;
      }
      return kReturned;
    case IntOperation::Subtract:
      if (!CheckedSub(left, right, result)) {
        return kIntegerOverflow;
      }
      return kReturned;
    case IntOperation::Multiply:
      if (!CheckedMul(left, right, result)) {
        return kIntegerOverflow;
      }
      return kReturned;
    case IntOperation::Divide:
      if (right == 0) {
        return kDivisionByZero;
      }
      if (left == std::numeric_limits<std::int64_t>::min() && right == -1) {
        return kIntegerOverflow;
      }
      *result = left / right;
      return kReturned;
    case IntOperation::Modulo:
      if (right == 0) {
        return kDivisionByZero;
      }
      *result = left == std::numeric_limits<std::int64_t>::min() && right == -1 ? 0 : left % right;
      return kReturned;
    case IntOperation::BitAnd:
      *result = left & right;
      return kReturned;
    case IntOperation::BitOr:
      *result = left | right;
      return kReturned;
    case IntOperation::BitXor:
      *result = left ^ right;
      return kReturned;
    case IntOperation::ShiftLeft:
      if (right < 0 || right >= kIntegerBitCount) {
        return kTypeError;
      }
      if (right != 0 && (left > (std::numeric_limits<std::int64_t>::max() >> right) ||
                         left < (std::numeric_limits<std::int64_t>::min() >> right))) {
        return kIntegerOverflow;
      }
      *result = left << right;
      return kReturned;
    case IntOperation::ShiftRight:
      if (right < 0 || right >= kIntegerBitCount) {
        return kTypeError;
      }
      *result = left >> right;
      return kReturned;
    case IntOperation::ShiftUnsignedRight:
      if (right < 0 || right >= kIntegerBitCount) {
        return kTypeError;
      }
      *result = static_cast<std::int64_t>(static_cast<std::uint64_t>(left) >> right);
      return kReturned;
  }
  return kTypeError;
}

}  // namespace

NativeFrame::NativeFrame(Isolate& isolate, std::size_t root_count)
    : roots_(root_count), root_range_(isolate.heap(), roots_.data(), roots_.size()) {}

NativeContext::NativeContext(Isolate& isolate) noexcept : isolate_(&isolate) {}

NativeContext::~NativeContext() {
  while (!frames_.empty()) {
    frames_.pop_back();
  }
}

std::size_t NativeContext::frame_count() const noexcept {
  return frames_.size();
}

std::span<Value> NativeContext::current_roots() noexcept {
  return frames_.empty() ? std::span<Value>() : frames_.back()->roots();
}

extern "C" std::uint8_t vela_native_frame_enter(NativeContext* context,
                                                std::uint64_t root_count) noexcept {
  if (context == nullptr || context->isolate_ == nullptr) {
    return static_cast<std::uint8_t>(ExecutionStatus::InvalidIsolate);
  }
  const auto count = static_cast<std::size_t>(root_count);
  std::unique_ptr<NativeFrame> frame(new (std::nothrow) NativeFrame(*context->isolate_, count));
  if (frame == nullptr) {
    return static_cast<std::uint8_t>(ExecutionStatus::OutOfMemory);
  }
  context->frames_.push_back(std::move(frame));
  return kReturned;
}

extern "C" void vela_native_frame_exit(NativeContext* context) noexcept {
  if (context != nullptr && !context->frames_.empty()) {
    context->frames_.pop_back();
  }
}

extern "C" std::uint8_t vela_native_copy_argument(NativeValue* destination,
                                                  const NativeValue* arguments,
                                                  std::uint64_t argument_count,
                                                  std::uint64_t index,
                                                  NativeValueKind expected) noexcept {
  if (destination == nullptr || arguments == nullptr || index >= argument_count) {
    return static_cast<std::uint8_t>(ExecutionStatus::InvalidEntry);
  }
  const auto argument = std::span(arguments, argument_count).subspan(index, 1).front();
  if (!IsKind(argument, expected)) {
    return kTypeError;
  }
  *destination = argument;
  return kReturned;
}

extern "C" std::uint8_t vela_native_copy(NativeValue* destination,
                                         const NativeValue* source) noexcept {
  if (destination == nullptr || source == nullptr) {
    return static_cast<std::uint8_t>(ExecutionStatus::InvalidEntry);
  }
  *destination = *source;
  return kReturned;
}

extern "C" std::uint8_t vela_native_store_null(NativeValue* destination) noexcept {
  if (destination == nullptr) {
    return static_cast<std::uint8_t>(ExecutionStatus::InvalidEntry);
  }
  *destination = {};
  return kReturned;
}

extern "C" std::uint8_t vela_native_store_bool(NativeValue* destination, bool value) noexcept {
  return destination == nullptr ? static_cast<std::uint8_t>(ExecutionStatus::InvalidEntry)
                                : Store(destination, NativeValueKind::Boolean, value ? 1 : 0);
}

extern "C" std::uint8_t vela_native_store_int(NativeValue* destination,
                                              std::int64_t value) noexcept {
  return destination == nullptr ? static_cast<std::uint8_t>(ExecutionStatus::InvalidEntry)
                                : StoreInteger(destination, value);
}

extern "C" std::uint8_t vela_native_store_double(NativeValue* destination, double value) noexcept {
  return destination == nullptr ? static_cast<std::uint8_t>(ExecutionStatus::InvalidEntry)
                                : StoreDouble(destination, value);
}

extern "C" std::uint8_t vela_native_store_result(NativeValue* destination,
                                                 const NativeValue* source,
                                                 NativeValueKind expected) noexcept {
  if (destination == nullptr || source == nullptr) {
    return static_cast<std::uint8_t>(ExecutionStatus::InvalidEntry);
  }
  if (!IsKind(*source, expected)) {
    return kTypeError;
  }
  *destination = *source;
  return kReturned;
}

extern "C" std::uint8_t vela_native_int_binary(NativeValue* destination,
                                               const NativeValue* left,
                                               const NativeValue* right,
                                               std::uint8_t operation) noexcept {
  if (destination == nullptr || left == nullptr || right == nullptr ||
      !IsKind(*left, NativeValueKind::Integer) || !IsKind(*right, NativeValueKind::Integer)) {
    return kTypeError;
  }
  const auto lhs = ReadInteger(*left);
  const auto rhs = ReadInteger(*right);
  std::int64_t result = 0;
  const auto status =
      ApplyIntegerOperation(static_cast<IntOperation>(operation), lhs, rhs, &result);
  if (status != kReturned) {
    return status;
  }
  return StoreInteger(destination, result);
}

extern "C" std::uint8_t vela_native_int_unary(NativeValue* destination,
                                              const NativeValue* operand,
                                              std::uint8_t operation) noexcept {
  if (destination == nullptr || operand == nullptr || !IsKind(*operand, NativeValueKind::Integer)) {
    return kTypeError;
  }
  const auto value = ReadInteger(*operand);
  switch (static_cast<UnaryOperation>(operation)) {
    case UnaryOperation::Negate:
      if (value == std::numeric_limits<std::int64_t>::min())
        return kIntegerOverflow;
      return StoreInteger(destination, -value);
    case UnaryOperation::BitNot:
      return StoreInteger(destination, ~value);
    case UnaryOperation::Not:
      return kTypeError;
  }
  return kTypeError;
}

extern "C" std::uint8_t vela_native_double_binary(NativeValue* destination,
                                                  const NativeValue* left,
                                                  const NativeValue* right,
                                                  std::uint8_t operation) noexcept {
  if (destination == nullptr || left == nullptr || right == nullptr ||
      !IsKind(*left, NativeValueKind::Double) || !IsKind(*right, NativeValueKind::Double)) {
    return kTypeError;
  }
  const auto lhs = ReadDouble(*left);
  const auto rhs = ReadDouble(*right);
  switch (static_cast<IntOperation>(operation)) {
    case IntOperation::Add:
      return StoreDouble(destination, lhs + rhs);
    case IntOperation::Subtract:
      return StoreDouble(destination, lhs - rhs);
    case IntOperation::Multiply:
      return StoreDouble(destination, lhs * rhs);
    case IntOperation::Divide:
      if (rhs == 0.0)
        return kDivisionByZero;
      return StoreDouble(destination, lhs / rhs);
    default:
      return kTypeError;
  }
}

extern "C" std::uint8_t vela_native_double_unary(NativeValue* destination,
                                                 const NativeValue* operand,
                                                 std::uint8_t operation) noexcept {
  if (destination == nullptr || operand == nullptr || !IsKind(*operand, NativeValueKind::Double)) {
    return kTypeError;
  }
  const auto value = ReadDouble(*operand);
  switch (static_cast<UnaryOperation>(operation)) {
    case UnaryOperation::Negate:
      return StoreDouble(destination, -value);
    default:
      return kTypeError;
  }
}

extern "C" std::uint8_t vela_native_bool_binary(NativeValue* destination,
                                                const NativeValue* left,
                                                const NativeValue* right,
                                                std::uint8_t operation) noexcept {
  if (destination == nullptr || left == nullptr || right == nullptr ||
      !IsKind(*left, NativeValueKind::Boolean) || !IsKind(*right, NativeValueKind::Boolean)) {
    return kTypeError;
  }
  const auto lhs = left->payload != 0;
  const auto rhs = right->payload != 0;
  if (operation > 1) {
    return kTypeError;
  }
  return Store(destination, NativeValueKind::Boolean,
               (operation == 0 ? lhs && rhs : lhs || rhs) ? 1 : 0);
}

extern "C" std::uint8_t vela_native_compare(NativeValue* destination,
                                            const NativeValue* left,
                                            const NativeValue* right,
                                            std::uint8_t operation) noexcept {
  if (destination == nullptr || left == nullptr || right == nullptr)
    return kTypeError;
  bool result = false;
  const auto compare = static_cast<CompareOperation>(operation);
  if (IsKind(*left, NativeValueKind::Integer) && IsKind(*right, NativeValueKind::Integer)) {
    const auto lhs = ReadInteger(*left);
    const auto rhs = ReadInteger(*right);
    switch (compare) {
      case CompareOperation::Equal:
        result = lhs == rhs;
        break;
      case CompareOperation::NotEqual:
        result = lhs != rhs;
        break;
      case CompareOperation::Less:
        result = lhs < rhs;
        break;
      case CompareOperation::LessEqual:
        result = lhs <= rhs;
        break;
      case CompareOperation::Greater:
        result = lhs > rhs;
        break;
      case CompareOperation::GreaterEqual:
        result = lhs >= rhs;
        break;
    }
  } else if (IsKind(*left, NativeValueKind::Double) && IsKind(*right, NativeValueKind::Double)) {
    const auto lhs = ReadDouble(*left);
    const auto rhs = ReadDouble(*right);
    switch (compare) {
      case CompareOperation::Equal:
        result = lhs == rhs;
        break;
      case CompareOperation::NotEqual:
        result = lhs != rhs;
        break;
      case CompareOperation::Less:
        result = lhs < rhs;
        break;
      case CompareOperation::LessEqual:
        result = lhs <= rhs;
        break;
      case CompareOperation::Greater:
        result = lhs > rhs;
        break;
      case CompareOperation::GreaterEqual:
        result = lhs >= rhs;
        break;
    }
  } else if (compare == CompareOperation::Equal || compare == CompareOperation::NotEqual) {
    result = left->kind == right->kind && left->payload == right->payload;
    if (compare == CompareOperation::NotEqual)
      result = !result;
  } else {
    return kTypeError;
  }
  return Store(destination, NativeValueKind::Boolean, result ? 1 : 0);
}

extern "C" std::uint8_t vela_native_is_false(const NativeValue* value, bool* result) noexcept {
  if (value == nullptr || result == nullptr || !IsKind(*value, NativeValueKind::Boolean)) {
    return kTypeError;
  }
  *result = value->payload == 0;
  return kReturned;
}

extern "C" std::uint8_t vela_native_is_null(const NativeValue* value, bool* result) noexcept {
  if (value == nullptr || result == nullptr) {
    return static_cast<std::uint8_t>(ExecutionStatus::InvalidEntry);
  }
  *result = IsKind(*value, NativeValueKind::Null);
  return kReturned;
}

}  // namespace vela::runtime
