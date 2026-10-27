#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_function.h"
#include "runtime/value.h"

namespace vela::runtime {

inline constexpr std::uint32_t kNativeAbiVersion = 1;
inline constexpr std::size_t kNativeValueBytes = 16;
inline constexpr std::size_t kNativeValuePayloadOffset = 0;
inline constexpr std::size_t kNativeValueKindOffset = 8;
inline constexpr std::size_t kNativeValueReservedBytes = 7;

enum class NativeValueKind : std::uint8_t {
  Null,
  Boolean,
  Integer,
  Double,
  Object,
  String,
};

// This is the only value representation visible to generated code. The payload is
// deliberately fixed-width; the kind byte is checked at every host boundary.
struct NativeValue final {
  std::uint64_t payload = 0;
  NativeValueKind kind = NativeValueKind::Null;
  std::array<std::uint8_t, kNativeValueReservedBytes> reserved = {};
};
static_assert(sizeof(NativeValue) == kNativeValueBytes);
static_assert(offsetof(NativeValue, payload) == kNativeValuePayloadOffset);
static_assert(offsetof(NativeValue, kind) == kNativeValueKindOffset);

class NativeFrame final {
 public:
  NativeFrame(Isolate& isolate, std::size_t root_count);
  ~NativeFrame() = default;

  NativeFrame(const NativeFrame&) = delete;
  NativeFrame& operator=(const NativeFrame&) = delete;
  NativeFrame(NativeFrame&&) = delete;
  NativeFrame& operator=(NativeFrame&&) = delete;

  [[nodiscard]] std::span<Value> roots() noexcept { return roots_; }

 private:
  std::vector<Value> roots_;
  RootRange root_range_;
};

// Helpers below are the stable C ABI used by generated code. They never allocate.
extern "C" std::uint8_t vela_native_frame_enter(NativeContext* context,
                                                std::uint64_t root_count) noexcept;
extern "C" void vela_native_frame_exit(NativeContext* context) noexcept;
extern "C" std::uint8_t vela_native_copy_argument(NativeValue* destination,
                                                  const NativeValue* arguments,
                                                  std::uint64_t argument_count,
                                                  std::uint64_t index,
                                                  NativeValueKind expected) noexcept;
extern "C" std::uint8_t vela_native_copy(NativeValue* destination,
                                         const NativeValue* source) noexcept;
extern "C" std::uint8_t vela_native_store_null(NativeValue* destination) noexcept;
extern "C" std::uint8_t vela_native_store_bool(NativeValue* destination, bool value) noexcept;
extern "C" std::uint8_t vela_native_store_int(NativeValue* destination,
                                              std::int64_t value) noexcept;
extern "C" std::uint8_t vela_native_store_double(NativeValue* destination, double value) noexcept;
extern "C" std::uint8_t vela_native_store_result(NativeValue* destination,
                                                 const NativeValue* source,
                                                 NativeValueKind expected) noexcept;
extern "C" std::uint8_t vela_native_int_binary(NativeValue* destination,
                                               const NativeValue* left,
                                               const NativeValue* right,
                                               std::uint8_t operation) noexcept;
extern "C" std::uint8_t vela_native_int_unary(NativeValue* destination,
                                              const NativeValue* operand,
                                              std::uint8_t operation) noexcept;
extern "C" std::uint8_t vela_native_double_binary(NativeValue* destination,
                                                  const NativeValue* left,
                                                  const NativeValue* right,
                                                  std::uint8_t operation) noexcept;
extern "C" std::uint8_t vela_native_double_unary(NativeValue* destination,
                                                 const NativeValue* operand,
                                                 std::uint8_t operation) noexcept;
extern "C" std::uint8_t vela_native_bool_binary(NativeValue* destination,
                                                const NativeValue* left,
                                                const NativeValue* right,
                                                std::uint8_t operation) noexcept;
extern "C" std::uint8_t vela_native_compare(NativeValue* destination,
                                            const NativeValue* left,
                                            const NativeValue* right,
                                            std::uint8_t operation) noexcept;
extern "C" std::uint8_t vela_native_is_false(const NativeValue* value, bool* result) noexcept;
extern "C" std::uint8_t vela_native_is_null(const NativeValue* value, bool* result) noexcept;

}  // namespace vela::runtime
