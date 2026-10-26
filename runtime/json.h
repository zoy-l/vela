#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {

class Isolate;

enum class JsonFailure : std::uint8_t { Decode, Encode, OutOfMemory, ConcurrentModification };

struct JsonRuntimeError final {
  JsonFailure failure = JsonFailure::Decode;
  std::size_t offset = 0;
  std::string message;
};

// source 的借用由调用者保根；返回图由 Isolate 拥有，不借用 source 或解析 arena。
[[nodiscard]] std::expected<Value, JsonRuntimeError> ParseJson(std::string_view source,
                                                               Isolate& isolate);

struct JsonEncodeFrame;
class JsonEncoder;
struct JsonEncoderDeleter final {
  void operator()(JsonEncoder* encoder) const;
};
using JsonEncoderPtr = std::unique_ptr<JsonEncoder, JsonEncoderDeleter>;

enum class JsonEncodeAction : std::uint8_t { Complete, Convert };

struct JsonEncodeStep final {
  JsonEncodeAction action = JsonEncodeAction::Complete;
  Value value;
};

// Convert 将控制权交回 VM，由普通调用帧执行公开的 toJson，再恢复同一个遍历状态。
class JsonEncoder final {
 public:
  JsonEncoder(Isolate& isolate, Value input);
  [[nodiscard]] static JsonEncoderPtr Create(Isolate& isolate, Value input);
  ~JsonEncoder();
  JsonEncoder(const JsonEncoder&) = delete;
  JsonEncoder& operator=(const JsonEncoder&) = delete;
  JsonEncoder(JsonEncoder&&) = delete;
  JsonEncoder& operator=(JsonEncoder&&) = delete;

  [[nodiscard]] std::expected<JsonEncodeStep, JsonRuntimeError> Advance();
  void ResumeConversion(Value result);
  [[nodiscard]] JsonRuntimeError ConversionError(std::string_view message) const;

 private:
  friend struct JsonEncoderDeleter;
  [[nodiscard]] std::expected<void, JsonRuntimeError> Append(std::string_view text);
  [[nodiscard]] std::expected<void, JsonRuntimeError> Quote(std::string_view text);
  [[nodiscard]] std::expected<void, JsonRuntimeError> Push(Value value);
  [[nodiscard]] std::expected<void, JsonRuntimeError> Initialize();
  [[nodiscard]] std::expected<void, JsonRuntimeError> EncodeScalar(Value value);
  [[nodiscard]] std::expected<std::optional<JsonEncodeStep>, JsonRuntimeError> EncodeValue();
  [[nodiscard]] std::expected<std::optional<JsonEncodeStep>, JsonRuntimeError> EnterObject(
      Value value);
  [[nodiscard]] std::expected<void, JsonRuntimeError> AdvanceArray();
  [[nodiscard]] std::expected<void, JsonRuntimeError> AdvanceMap();
  [[nodiscard]] std::expected<void, JsonRuntimeError> CheckElement(const ArrayLayout& layout) const;
  void Pop();
  [[nodiscard]] JsonRuntimeError Error(JsonFailure failure, std::string_view message) const;

  Heap* heap_;
  const CollectionLayout* active_type_;
  // pending、输出 builder、当前路径的身份集合均跨越 GC 和 VM 调用保根。
  std::array<Value, 3> values_;
  RootRange roots_;
  JsonEncodeFrame* frame_ = nullptr;
  bool pending_ = true;
};

}  // namespace vela::runtime
