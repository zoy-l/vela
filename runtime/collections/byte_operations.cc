#include "runtime/collections/byte_operations.h"
#include <optional>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <span>
#include <utility>

#include "base/builtin.h"
#include "base/byte_access.h"
#include "base/execution_status.h"
#include "runtime/collections/index_range.h"
#include "runtime/heap/heap.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_operations.h"

namespace vela::runtime {
namespace {

using Result = std::expected<Value, ExecutionStatus>;
using Bytes = std::span<std::uint8_t>;

static_assert(sizeof(float) == sizeof(std::uint32_t) && std::numeric_limits<float>::is_iec559);
static_assert(std::endian::native == std::endian::little ||
              std::endian::native == std::endian::big);
constexpr auto kHostByteOrder =
    std::endian::native == std::endian::little ? ByteOrder::Little : ByteOrder::Big;

bool IsByte(std::int64_t number) {
  return number >= 0 && number <= kMaximumByte;
}

std::expected<HeapObject*, ExecutionStatus> Owner(Value value) {
  auto* object = value.AsObjectOrNull();
  if (object == nullptr || !Heap::IsByteArray(*object)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return object;
}

std::expected<Bytes, ExecutionStatus> Access(Bytes bytes, Value offset, std::size_t width) {
  if (!offset.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto index = offset.AsInteger();
  if (index < 0 || width > bytes.size() || std::cmp_greater(index, bytes.size() - width)) {
    return std::unexpected(ExecutionStatus::IndexOutOfBounds);
  }
  return bytes.subspan(static_cast<std::size_t>(index), width);
}

Result NewBytes(Value length, Heap& heap) {
  if (!length.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto size = length.AsInteger();
  if (size < 0) {
    return std::unexpected(ExecutionStatus::InvalidNumericConversion);
  }
  if (std::cmp_greater(size, kMaximumByteArrayLength)) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  return AllocatedValue(heap.AllocateByteArray(static_cast<std::size_t>(size)));
}

Result FromArray(Value input, Heap& heap) {
  auto* input_object = input.AsObjectOrNull();
  if (input_object == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto& array = *input_object;
  const auto numbers = Heap::IntegerElements(array);
  if (!numbers) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (!std::ranges::all_of(*numbers, IsByte)) {
    return std::unexpected(ExecutionStatus::InvalidNumericConversion);
  }
  RootRange root(heap, &input, 1, std::nullopt);
  const auto result = AllocatedValue(heap.AllocateByteArray(numbers->size()));
  if (!result) {
    return result;
  }
  const auto bytes = Heap::ByteArrayBytes(*result->AsObject());
  std::ranges::transform(*numbers, bytes.begin(),
                         [](std::int64_t number) { return static_cast<std::uint8_t>(number); });
  return result;
}

Result Slice(HeapObject& owner, std::span<const Value> arguments, Heap& heap) {
  const auto first = arguments[1];
  const auto last = arguments[2];
  if (!first.IsInteger() || !last.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto input = Value::Object(&owner);
  RootRange root(heap, &input, 1, std::nullopt);
  const auto bytes = Heap::ByteArrayBytes(owner);
  const auto [start, end] = ClampIndexRange(first.AsInteger(), last.AsInteger(), bytes.size());
  const auto result = AllocatedValue(heap.AllocateByteArray(end - start));
  if (result) {
    std::ranges::copy(bytes.subspan(start, end - start),
                      Heap::ByteArrayBytes(*result->AsObject()).begin());
  }
  return result;
}

Result View(HeapObject& owner, std::span<const Value> arguments, Heap& heap) {
  const auto offset = arguments[1];
  const auto length = arguments[2];
  if (!offset.IsInteger() || (!length.IsInteger() && !length.IsNull())) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto index = offset.AsInteger();
  const auto size = Heap::ByteArrayBytes(owner).size();
  if (index < 0 || std::cmp_greater(index, size)) {
    return std::unexpected(ExecutionStatus::IndexOutOfBounds);
  }
  const auto remaining = size - static_cast<std::size_t>(index);
  // null 只表示编译器为省略 length 生成的内部默认值；公开参数类型仍为 int。
  const auto count = length.IsNull() ? static_cast<std::int64_t>(remaining) : length.AsInteger();
  if (count < 0 || std::cmp_greater(count, remaining)) {
    return std::unexpected(ExecutionStatus::IndexOutOfBounds);
  }
  return AllocatedValue(
      heap.AllocateByteData(owner, {
                                       .offset = static_cast<std::uint32_t>(index),
                                       .length = static_cast<std::uint32_t>(count),
                                   }));
}

std::expected<ByteOrder, ExecutionStatus> Order(Value value) {
  if (!value.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto order = value.AsInteger();
  if (order < 0 || order > static_cast<std::int64_t>(ByteOrder::Host)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (order == static_cast<std::int64_t>(ByteOrder::Host)) {
    return kHostByteOrder;
  }
  return static_cast<ByteOrder>(order);
}

std::uint64_t ReadBits(Bytes bytes, ByteOrder order) {
  std::uint64_t result = 0;
  std::memcpy(&result, bytes.data(), bytes.size());
  const auto shift = (sizeof(result) - bytes.size()) * kBitsPerByte;
  if constexpr (std::endian::native == std::endian::big) {
    result >>= shift;
  }
  if (order != kHostByteOrder) {
    result = std::byteswap(result) >> shift;
  }
  return result;
}

void WriteBits(Bytes bytes, ByteOrder order, std::uint64_t bits) {
  const auto shift = (sizeof(bits) - bytes.size()) * kBitsPerByte;
  if (order != kHostByteOrder) {
    bits = std::byteswap(bits) >> shift;
  }
  if constexpr (std::endian::native == std::endian::big) {
    bits <<= shift;
  }
  std::memcpy(bytes.data(), &bits, bytes.size());
}

std::uint64_t RoundRight(std::uint64_t bits, std::uint32_t shift) {
  constexpr auto kBitWidth = std::numeric_limits<std::uint64_t>::digits;
  if (shift >= kBitWidth) {
    return 0;
  }
  const auto whole = bits >> shift;
  const auto half = std::uint64_t{1} << (shift - 1);
  const auto remainder = bits & ((half << 1U) - 1);
  return whole + (remainder > half || (remainder == half && (whole & 1U) != 0) ? 1U : 0U);
}

// 用无符号位运算固定最近偶数舍入，不读取或修改宿主浮点舍入环境。
std::uint32_t Float32Bits(double value) {
  constexpr std::uint32_t kDoubleFraction = 52;
  constexpr std::uint32_t kFloatFraction = 23;
  constexpr std::uint64_t kDoubleFractionMask = (std::uint64_t{1} << kDoubleFraction) - 1;
  constexpr std::uint32_t kDoubleExponentMask = 0x7ff;
  constexpr std::uint32_t kDoubleBias = 1023;
  constexpr std::uint32_t kFloatSign = 0x80000000;
  constexpr std::uint32_t kFloatInfinity = 0x7f800000;
  constexpr std::uint32_t kFloatQuietNan = 0x00400000;
  constexpr std::int32_t kMinimumExponent = -126;
  constexpr std::int32_t kMaximumExponent = 127;
  const auto bits = std::bit_cast<std::uint64_t>(value);
  const auto sign = static_cast<std::uint32_t>(bits >> 32U) & kFloatSign;
  const auto exponent = static_cast<std::uint32_t>((bits >> kDoubleFraction) & kDoubleExponentMask);
  const auto fraction = bits & kDoubleFractionMask;
  if (exponent == kDoubleExponentMask) {
    return sign | kFloatInfinity | (fraction == 0 ? 0U : kFloatQuietNan);
  }
  if (exponent == 0) {
    return sign;
  }
  const auto power = static_cast<std::int32_t>(exponent) - static_cast<std::int32_t>(kDoubleBias);
  if (power > kMaximumExponent) {
    return sign | kFloatInfinity;
  }
  const auto mantissa = fraction | (std::uint64_t{1} << kDoubleFraction);
  constexpr auto kFractionShift = kDoubleFraction - kFloatFraction;
  if (power < kMinimumExponent) {
    const auto shift = static_cast<std::uint32_t>(kMinimumExponent - power) + kFractionShift;
    return sign | static_cast<std::uint32_t>(RoundRight(mantissa, shift));
  }
  const auto rounded = static_cast<std::uint32_t>(RoundRight(mantissa, kFractionShift));
  const auto field = static_cast<std::uint32_t>(power - kMinimumExponent) << kFloatFraction;
  return sign | (field + rounded);
}

Value Decode(const ByteAccess& access, std::uint64_t bits) {
  if (access.number == ByteNumber::Float) {
    return Value::Double(access.width == sizeof(float) ? static_cast<double>(std::bit_cast<float>(
                                                             static_cast<std::uint32_t>(bits)))
                                                       : std::bit_cast<double>(bits));
  }
  const auto width = access.width * kBitsPerByte;
  if (access.number == ByteNumber::Signed && access.width < sizeof(std::int64_t) &&
      (bits & (std::uint64_t{1} << (width - 1))) != 0) {
    bits |= std::numeric_limits<std::uint64_t>::max() << width;
  }
  return Value::Integer(std::bit_cast<std::int64_t>(bits));
}

std::expected<std::uint64_t, ExecutionStatus> Encode(const ByteAccess& access, Value value) {
  if (access.number == ByteNumber::Float) {
    if (!value.IsDouble()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    const auto number = value.AsDouble();
    return access.width == sizeof(float) ? Float32Bits(number)
                                         : std::bit_cast<std::uint64_t>(number);
  }
  if (!value.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto number = value.AsInteger();
  if (access.width < sizeof(std::int64_t)) {
    const auto bits = access.width * kBitsPerByte;
    const auto maximum = static_cast<std::int64_t>(
        (std::uint64_t{1} << (bits - (access.number == ByteNumber::Signed ? 1 : 0))) - 1);
    const auto minimum = access.number == ByteNumber::Signed ? -maximum - 1 : 0;
    if (number < minimum || number > maximum) {
      return std::unexpected(ExecutionStatus::InvalidNumericConversion);
    }
  }
  return std::bit_cast<std::uint64_t>(number);
}

Result TypedAccess(const ByteAccess& access,
                   Builtin builtin,
                   std::span<const Value> arguments,
                   Bytes storage) {
  const auto bytes = Access(storage, arguments[1], access.width);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  const auto order = access.width == 1 ? std::expected<ByteOrder, ExecutionStatus>(ByteOrder::Big)
                                       : Order(arguments.back());
  if (!order) {
    return std::unexpected(order.error());
  }
  if (builtin == access.get) {
    return Decode(access, ReadBits(*bytes, *order));
  }
  const auto bits = Encode(access, arguments[2]);
  if (!bits) {
    return std::unexpected(bits.error());
  }
  WriteBits(*bytes, *order, *bits);
  return Value::Null();
}

}  // namespace

std::expected<Value, ExecutionStatus> ExecuteByteBuiltin(Builtin builtin,
                                                         std::span<const Value> arguments,
                                                         Heap& heap) {
  if (builtin == Builtin::NewByteArray) {
    return NewBytes(arguments.front(), heap);
  }
  if (builtin == Builtin::ByteArrayFrom) {
    return FromArray(arguments.front(), heap);
  }
  const auto* access = FindByteAccess(builtin);
  if (builtin == Builtin::ByteDataLength || access != nullptr) {
    const auto input = arguments.front();
    auto* object = input.AsObjectOrNull();
    if (object == nullptr || !Heap::IsByteData(*object)) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    const auto bytes = Heap::ByteDataBytes(*object);
    return builtin == Builtin::ByteDataLength
               ? Result(Value::Integer(static_cast<std::int64_t>(bytes.size())))
               : TypedAccess(*access, builtin, arguments, bytes);
  }
  const auto owner = Owner(arguments.front());
  if (!owner) {
    return std::unexpected(owner.error());
  }
  if (builtin == Builtin::ByteDataView) {
    return View(**owner, arguments, heap);
  }
  if (builtin == Builtin::ByteArraySlice) {
    return Slice(**owner, arguments, heap);
  }
  const auto bytes = Heap::ByteArrayBytes(**owner);
  if (builtin == Builtin::ByteArrayLength) {
    return Value::Integer(static_cast<std::int64_t>(bytes.size()));
  }
  const auto element = Access(bytes, arguments[1], 1);
  if (!element) {
    return std::unexpected(element.error());
  }
  switch (builtin) {
    case Builtin::ByteArrayGetIndex:
      return Value::Integer(element->front());
    case Builtin::ByteArraySetIndex:
      break;
    default:
      std::unreachable();
  }
  const auto value = arguments[2];
  if (!value.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto number = value.AsInteger();
  if (!IsByte(number)) {
    return std::unexpected(ExecutionStatus::InvalidNumericConversion);
  }
  element->front() = static_cast<std::uint8_t>(number);
  return Value::Null();
}

}  // namespace vela::runtime
