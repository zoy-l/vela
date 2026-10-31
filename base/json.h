#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <yyjson.h>

namespace vela {

struct JsonError final {
  std::size_t offset = 0;
  std::string message;
  bool out_of_memory = false;
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

struct JsonBufferDeleter final {
  std::optional<yyjson_alc> allocator = std::nullopt;
  void operator()(char* buffer) const noexcept;
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

// document 独占节点和字符串；自定义 allocator 的上下文须覆盖 document 生命周期。
class JsonDocument final {
 public:
  [[nodiscard]] static std::expected<JsonDocument, JsonError> Parse(
      std::string_view text,
      yyjson_read_flag flags = YYJSON_READ_NOFLAG,
      const yyjson_alc* allocator = nullptr);
  [[nodiscard]] static std::expected<JsonDocument, JsonError> Parse(
      std::span<const std::byte> bytes,
      yyjson_read_flag flags = YYJSON_READ_NOFLAG,
      const yyjson_alc* allocator = nullptr);
  JsonDocument(const JsonDocument&) = delete;
  JsonDocument& operator=(const JsonDocument&) = delete;
  JsonDocument(JsonDocument&&) noexcept = default;
  JsonDocument& operator=(JsonDocument&&) noexcept = default;
  ~JsonDocument() = default;

  [[nodiscard]] const yyjson_val* root() const noexcept;
  [[nodiscard]] yyjson_val* root() noexcept;
  // value 须为本 document 的 raw 数值节点；INSITU 保留 token 在独占原文缓冲区中的位置。
  [[nodiscard]] std::size_t RawOffset(const yyjson_val& value) const noexcept;

 private:
  JsonDocument() = default;
  std::unique_ptr<char, JsonBufferDeleter> storage_;
  std::unique_ptr<yyjson_doc, JsonDeleter> document_;
};

}  // namespace vela
