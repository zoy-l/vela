#include "base/json.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>

#include <yyjson.h>

namespace vela {

const yyjson_val* GetJsonField(const yyjson_val* object, std::string_view key) noexcept {
  return yyjson_obj_getn(object, key.data(), key.size());
}

yyjson_mut_val* GetMutableJsonField(yyjson_mut_val* object, std::string_view key) noexcept {
  return yyjson_mut_obj_getn(object, key.data(), key.size());
}

void AddJsonField(yyjson_mut_doc* document,
                  yyjson_mut_val* object,
                  std::string_view key,
                  yyjson_mut_val* value) {
  yyjson_mut_obj_add(object, yyjson_mut_strncpy(document, key.data(), key.size()), value);
}

void MutableJsonDeleter::operator()(yyjson_mut_doc* document) const noexcept {
  yyjson_mut_doc_free(document);
}

JsonOutput::JsonOutput() : document_(yyjson_mut_doc_new(nullptr)) {
  yyjson_mut_doc_set_root(document_.get(), yyjson_mut_obj(document_.get()));
}

yyjson_mut_val* JsonOutput::root() const noexcept {
  return yyjson_mut_doc_get_root(document_.get());
}

std::expected<std::string, JsonError> JsonOutput::Serialize() const {
  std::size_t length = 0;
  const std::unique_ptr<char, decltype(&std::free)> bytes(
      yyjson_mut_write(document_.get(), 0, &length), &std::free);
  if (bytes == nullptr) {
    return std::unexpected(JsonError{.message = "cannot serialize JSON"});
  }
  return std::string(bytes.get(), length);
}

void JsonDeleter::operator()(yyjson_doc* document) const noexcept {
  yyjson_doc_free(document);
}

void JsonBufferDeleter::operator()(char* buffer) const noexcept {
  if (allocator) {
    allocator->free(allocator->ctx, buffer);
  } else {
    ::operator delete(buffer);
  }
}

std::expected<JsonDocument, JsonError> JsonDocument::Parse(std::string_view text,
                                                           yyjson_read_flag flags,
                                                           const yyjson_alc* allocator) {
  JsonDocument result;
  if (text.size() > std::numeric_limits<std::size_t>::max() - YYJSON_PADDING_SIZE) {
    return std::unexpected(JsonError{.message = "JSON input is too large", .out_of_memory = true});
  }
  const auto size = text.size() + YYJSON_PADDING_SIZE;
  if (allocator != nullptr) {
    result.storage_.get_deleter().allocator = *allocator;
  }
  result.storage_.reset(static_cast<char*>(allocator != nullptr
                                               ? allocator->malloc(allocator->ctx, size)
                                               : ::operator new(size, std::nothrow)));
  if (!result.storage_) {
    return std::unexpected(
        JsonError{.message = "JSON input allocation failed", .out_of_memory = true});
  }
  std::ranges::copy(text, result.storage_.get());
  std::ranges::fill(std::span(result.storage_.get(), size).last(YYJSON_PADDING_SIZE), '\0');
  yyjson_read_err error{};
  // INSITU 只修改独占缓冲区；移动转移缓冲区和 arena，节点中的借用地址保持稳定。
  result.document_.reset(yyjson_read_opts(result.storage_.get(), text.size(),
                                          flags | YYJSON_READ_INSITU, allocator, &error));
  if (result.document_ == nullptr) {
    return std::unexpected(JsonError{
        .offset = error.pos,
        .message = error.msg,
        .out_of_memory = error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION,
    });
  }
  return result;
}

const yyjson_val* JsonDocument::root() const noexcept {
  return yyjson_doc_get_root(document_.get());
}

yyjson_val* JsonDocument::root() noexcept {
  return yyjson_doc_get_root(document_.get());
}

std::size_t JsonDocument::RawOffset(const yyjson_val& value) const noexcept {
  return static_cast<std::size_t>(yyjson_get_raw(&value) - storage_.get());
}

}  // namespace vela
