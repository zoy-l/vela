#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "base/execution_status.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {

class Isolate;

struct JsonRuntimeError final {
  ExecutionStatus status = ExecutionStatus::JsonDecodeError;
  std::size_t offset = 0;
  std::string message;
};

// source 的借用由调用者保根；返回图由 Isolate 拥有，不借用 source 或解析 arena。
[[nodiscard]] std::expected<Value, JsonRuntimeError> ParseJson(std::string_view source,
                                                               Isolate& isolate);

struct JsonEncodeFrame;
enum class JsonFrameKind : std::uint8_t;

// Convert 将控制权交回 VM，由普通调用帧执行公开的 toJson，再恢复同一个遍历状态。
class JsonEncoder final {
 public:
  JsonEncoder(Isolate& isolate, Value input);
  ~JsonEncoder();
  JsonEncoder(const JsonEncoder&) = delete;
  JsonEncoder& operator=(const JsonEncoder&) = delete;
  JsonEncoder(JsonEncoder&&) = delete;
  JsonEncoder& operator=(JsonEncoder&&) = delete;

  // string 是最终结果；object 是需要 VM 调用 toJson 的接收者。
  [[nodiscard]] std::expected<Value, JsonRuntimeError> Advance();
  void ResumeConversion(Value result);
  [[nodiscard]] JsonRuntimeError ConversionError(std::string_view message) const;

 private:
  [[nodiscard]] std::expected<void, JsonRuntimeError> Append(std::string_view text);
  [[nodiscard]] std::expected<void, JsonRuntimeError> Quote(std::string_view text);
  [[nodiscard]] std::expected<void, JsonRuntimeError> Push(Value value,
                                                           JsonFrameKind kind,
                                                           const ArrayLayout* elements);
  [[nodiscard]] std::expected<void, JsonRuntimeError> Initialize();
  [[nodiscard]] std::expected<void, JsonRuntimeError> EncodeScalar(Value value);
  [[nodiscard]] std::expected<std::optional<Value>, JsonRuntimeError> EncodeValue();
  [[nodiscard]] std::expected<std::optional<Value>, JsonRuntimeError> EnterObject(Value value);
  [[nodiscard]] std::expected<void, JsonRuntimeError> AdvanceArray();
  [[nodiscard]] std::expected<void, JsonRuntimeError> AdvanceMap();
  [[nodiscard]] std::expected<void, JsonRuntimeError> CheckElement(const ArrayLayout& layout) const;
  void Pop();
  [[nodiscard]] JsonRuntimeError Error(ExecutionStatus status, std::string_view message) const;

  static constexpr std::size_t kPending = 0;
  static constexpr std::size_t kOutput = 1;
  static constexpr std::size_t kActive = 2;
  static constexpr std::size_t kRootCount = 3;
  Heap* heap_;
  const CollectionLayout* active_type_;
  // pending、输出 builder、当前路径的身份集合均跨越 GC 和 VM 调用保根。
  std::array<Value, kRootCount> values_;
  RootRange roots_;
  JsonEncodeFrame* frame_ = nullptr;
  JsonEncodeFrame* spare_ = nullptr;
  bool pending_ = true;
};

}  // namespace vela::runtime
