#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <string_view>

#include <yyjson.h>

#include "base/json.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/native_buffer.h"
#include "runtime/json/json.h"
#include "runtime/value/value.h"

namespace vela::runtime {

[[nodiscard]] std::expected<JsonDocument, JsonRuntimeError> ReadJSONDocument(
    std::string_view source,
    Heap& heap);
[[nodiscard]] std::expected<Value, JsonRuntimeError> ReadJSONNumber(std::string_view token);

// 会话独占解析 arena 和索引；所有 view 借用本对象，结束时一起释放，不进入托管结果图。
class ParsedJSON final {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<ParsedJSON>, JsonRuntimeError> Parse(
      std::string_view source,
      Heap& heap);
  [[nodiscard]] yyjson_val* root() const noexcept { return root_; }
  [[nodiscard]] std::size_t size() const noexcept { return document_.value_count(); }
  [[nodiscard]] std::size_t Offset(const yyjson_val* value) const noexcept;
  [[nodiscard]] Value Number(const yyjson_val* value) const noexcept;
  [[nodiscard]] yyjson_val* Field(const yyjson_val* object, std::string_view key) const noexcept;
  [[nodiscard]] bool IsFirstKey(const yyjson_val* object, const yyjson_val* key) const noexcept;

 private:
  struct Node final {
    std::size_t offset = 0;
    Value number;
  };
  struct Member final {
    const yyjson_val* object = nullptr;
    const yyjson_val* key = nullptr;
    yyjson_val* value = nullptr;
  };
  ParsedJSON(JsonDocument document, Heap& heap);
  [[nodiscard]] std::expected<void, JsonRuntimeError> Index(std::string_view source);
  [[nodiscard]] std::size_t Slot(const yyjson_val* object, std::string_view key) const noexcept;

  JsonDocument document_;
  yyjson_val* root_;
  NativeBuffer<Node> nodes_;
  NativeBuffer<Member> members_;
};

}  // namespace vela::runtime
