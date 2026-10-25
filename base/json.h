#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <yyjson.h>

namespace vela {

struct JsonError final {
  std::size_t offset = 0;
  std::string message;
};

[[nodiscard]] const yyjson_val* GetJsonField(const yyjson_val* object,
                                             std::string_view key) noexcept;
[[nodiscard]] yyjson_mut_val* GetMutableJsonField(yyjson_mut_val* object,
                                                  std::string_view key) noexcept;
// key 复制到 arena，调用方可传临时文本；读取接口始终按长度查找。
void AddJsonField(yyjson_mut_doc* document,
                  yyjson_mut_val* object,
                  std::string_view key,
                  yyjson_mut_val* value);

struct JsonDeleter final {
  void operator()(yyjson_doc* document) const noexcept;
};

struct MutableJsonDeleter final {
  void operator()(yyjson_mut_doc* document) const noexcept;
};

// 结构化输出独占 yyjson arena；节点与复制的文本均随 document 一起释放。
class JsonOutput final {
 public:
  JsonOutput();
  [[nodiscard]] yyjson_mut_doc* document() const noexcept { return document_.get(); }
  [[nodiscard]] yyjson_mut_val* root() const noexcept;
  [[nodiscard]] std::expected<std::string, JsonError> Serialize() const;

 private:
  std::unique_ptr<yyjson_mut_doc, MutableJsonDeleter> document_;
};

// in-situ 字符串视图借用 storage_；移动转移堆缓冲，不使 yyjson 节点中的指针失效。
class JsonDocument final {
 public:
  [[nodiscard]] static std::expected<JsonDocument, JsonError> Parse(std::string_view text);
  JsonDocument(const JsonDocument&) = delete;
  JsonDocument& operator=(const JsonDocument&) = delete;
  JsonDocument(JsonDocument&&) noexcept = default;
  JsonDocument& operator=(JsonDocument&&) noexcept = default;
  ~JsonDocument() = default;

  [[nodiscard]] const yyjson_val* root() const noexcept;

 private:
  JsonDocument() = default;
  std::vector<char> storage_;
  std::unique_ptr<yyjson_doc, JsonDeleter> document_;
};

}  // namespace vela
