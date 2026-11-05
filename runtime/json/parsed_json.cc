#include "runtime/json/parsed_json.h"

#include <cstddef>
#include <expected>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <utility>

#include <absl/hash/hash.h>
#include <yyjson.h>

#include "base/execution_status.h"
#include "base/json.h"
#include "runtime/heap/heap.h"
#include "runtime/json/json.h"
#include "runtime/json/json_path.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

std::size_t TokenEnd(std::string_view source, std::size_t cursor) {
  const auto first = source[cursor++];
  if (first == '{' || first == '[') {
    return cursor;
  }
  if (first == '"') {
    while (source[cursor] != '"') {
      if (source[cursor++] == '\\') {
        ++cursor;
      }
    }
    return cursor + 1;
  }
  while (cursor < source.size() && !std::string_view(" \t\n\r,]}").contains(source[cursor])) {
    ++cursor;
  }
  return cursor;
}

JsonRuntimeError WithErrorPath(JsonRuntimeError error, std::string_view source, Heap& heap) {
  auto path = JSONPathAtOffset(source, error.offset, heap);
  if (!path) {
    error.status = path.error();
  } else {
    error.path = std::move(*path);
  }
  return error;
}

}  // namespace

ParsedJSON::ParsedJSON(JsonDocument document, Heap& heap)
    : document_(std::move(document)),
      root_(document_.root()),
      nodes_(heap, size()),
      members_(heap, (size() * 2) + 1) {}

std::expected<std::unique_ptr<ParsedJSON>, JsonRuntimeError> ParsedJSON::Parse(
    std::string_view source,
    Heap& heap) {
  auto document = ReadJSONDocument(source, heap);
  if (!document) {
    auto error = document.error();
    if (error.status == ExecutionStatus::JsonDecodeError) {
      error = WithErrorPath(std::move(error), source, heap);
    }
    return std::unexpected(std::move(error));
  }
  std::unique_ptr<ParsedJSON> parsed(new (std::nothrow) ParsedJSON(std::move(*document), heap));
  if (parsed == nullptr || !parsed->nodes_.IsAllocated() || !parsed->members_.IsAllocated()) {
    return std::unexpected(JsonRuntimeError{
        .status = ExecutionStatus::OutOfMemory,
        .message = "JSON index allocation failed",
        .expected = {},
        .actual = {},
    });
  }
  if (auto indexed = parsed->Index(source); !indexed) {
    return std::unexpected(WithErrorPath(std::move(indexed.error()), source, heap));
  }
  return parsed;
}

std::size_t ParsedJSON::Offset(const yyjson_val* value) const noexcept {
  return nodes_.values()[static_cast<std::size_t>(value - root_)].offset;
}

Value ParsedJSON::Number(const yyjson_val* value) const noexcept {
  return nodes_.values()[static_cast<std::size_t>(value - root_)].number;
}

std::size_t ParsedJSON::Slot(const yyjson_val* object, std::string_view key) const noexcept {
  const auto members = members_.values();
  auto slot = absl::HashOf(object, key) % members.size();
  while (members[slot].object != nullptr &&
         (members[slot].object != object ||
          std::string_view(yyjson_get_str(members[slot].key), yyjson_get_len(members[slot].key)) !=
              key)) {
    slot = (slot + 1) % members.size();
  }
  return slot;
}

yyjson_val* ParsedJSON::Field(const yyjson_val* object, std::string_view key) const noexcept {
  return members_.values()[Slot(object, key)].value;
}

bool ParsedJSON::IsFirstKey(const yyjson_val* object, const yyjson_val* key) const noexcept {
  return members_.values()[Slot(object, {yyjson_get_str(key), yyjson_get_len(key)})].key == key;
}

std::expected<void, JsonRuntimeError> ParsedJSON::Index(std::string_view source) {
  const auto nodes = nodes_.values();
  const auto members = members_.values();
  std::uninitialized_value_construct_n(nodes.data(), nodes.size());
  std::uninitialized_value_construct_n(members.data(), members.size());
  std::size_t cursor = 0;
  const std::span tokens(root_, nodes.size());
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    cursor = source.find_first_not_of(" \t\n\r,:]}", cursor);
    auto& metadata = nodes[index];
    metadata.offset = cursor;
    cursor = TokenEnd(source, cursor);
    auto* value = &tokens[index];
    if (yyjson_is_raw(value)) {
      const auto number = ReadJSONNumber({yyjson_get_raw(value), yyjson_get_len(value)});
      if (!number) {
        auto error = number.error();
        error.offset = metadata.offset;
        return std::unexpected(std::move(error));
      }
      metadata.number = *number;
    }
    if (yyjson_is_obj(value)) {
      auto iterator = yyjson_obj_iter_with(value);
      while (auto* key = yyjson_obj_iter_next(&iterator)) {
        auto& member = members[Slot(value, {yyjson_get_str(key), yyjson_get_len(key)})];
        if (member.object == nullptr) {
          member.object = value;
          member.key = key;
        }
        member.value = yyjson_obj_iter_get_val(key);
      }
    }
  }
  return {};
}

}  // namespace vela::runtime
