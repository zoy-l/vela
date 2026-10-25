#include "base/json.h"

#include <cstddef>
#include <cstdlib>
#include <expected>
#include <memory>
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

std::expected<JsonDocument, JsonError> JsonDocument::Parse(std::string_view text) {
  JsonDocument result;
  result.storage_.assign(text.begin(), text.end());
  result.storage_.resize(text.size() + YYJSON_PADDING_SIZE);
  yyjson_read_err error{};
  result.document_.reset(
      yyjson_read_opts(result.storage_.data(), text.size(), YYJSON_READ_INSITU, nullptr, &error));
  if (result.document_ == nullptr) {
    return std::unexpected(JsonError{.offset = error.pos, .message = error.msg});
  }
  return result;
}

const yyjson_val* JsonDocument::root() const noexcept {
  return yyjson_doc_get_root(document_.get());
}

}  // namespace vela
