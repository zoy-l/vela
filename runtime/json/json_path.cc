#include "runtime/json/json_path.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include <yyjson.h>

#include "base/execution_status.h"
#include "base/json.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/native_buffer.h"
#include "runtime/json/parsed_json.h"

namespace vela::runtime {
namespace {

bool IsIdentifierStart(char character) {
  return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
         character == '_';
}

struct PathFrame final {
  std::string_view key;
  std::size_t index = 0;
  bool object = false;
  bool awaiting_key = true;
};

std::size_t QuotedEnd(std::string_view source, std::size_t cursor) {
  while (++cursor < source.size()) {
    const auto character = source[cursor];
    if (character == '"') {
      return cursor + 1;
    }
    if (character == '\\') {
      ++cursor;
    }
  }
  return source.size();
}

void LocatePrefix(std::string_view source,
                  std::size_t offset,
                  std::span<PathFrame> frames,
                  std::size_t& depth) {
  for (std::size_t cursor = 0; cursor < offset; ++cursor) {
    const auto character = source[cursor];
    if (character == '"') {
      const auto end = QuotedEnd(source, cursor);
      if (end > offset) {
        return;
      }
      if (depth != 0) {
        auto& frame = frames[depth - 1];
        if (frame.object && frame.awaiting_key) {
          frame.key = source.substr(cursor, end - cursor);
          frame.awaiting_key = false;
        }
      }
      cursor = end - 1;
    } else if (character == '{' || character == '[') {
      frames[depth++] = {.key = {}, .object = character == '{'};
    } else if (character == '}' || character == ']') {
      if (depth != 0) {
        --depth;
      }
    } else if (character == ',' && depth != 0) {
      auto& frame = frames[depth - 1];
      ++frame.index;
      frame.key = {};
      frame.awaiting_key = true;
    }
  }
}

}  // namespace

void AppendJSONKey(std::string& path, std::string_view key) {
  bool identifier = !key.empty() && IsIdentifierStart(key.front());
  for (const auto character : key) {
    identifier =
        identifier && (IsIdentifierStart(character) || (character >= '0' && character <= '9'));
  }
  if (identifier) {
    path += '.';
    path += key;
    return;
  }
  path += "[\"";
  constexpr unsigned char kFirstPrintableByte = 0x20;
  for (const auto character : key) {
    const auto byte = static_cast<unsigned char>(character);
    if (character == '"' || character == '\\') {
      path += '\\';
      path += character;
    } else if (byte < kFirstPrintableByte) {
      const auto escape = JSONByteEscape(byte);
      path.append(escape.data(), escape.size());
    } else {
      path += character;
    }
  }
  path += "\"]";
}

std::expected<std::string, ExecutionStatus> JSONPathAtOffset(std::string_view source,
                                                             std::size_t offset,
                                                             Heap& heap) {
  const auto prefix = source.substr(0, offset);
  const auto count = std::ranges::count(prefix, '{') + std::ranges::count(prefix, '[');
  NativeBuffer<PathFrame> storage(heap, static_cast<std::size_t>(count));
  if (!storage.IsAllocated()) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  std::size_t depth = 0;
  LocatePrefix(source, prefix.size(), storage.values(), depth);
  std::string path = "$";
  for (const auto& frame : storage.values().first(depth)) {
    if (!frame.object) {
      path += '[' + std::to_string(frame.index) + ']';
      continue;
    }
    if (frame.key.empty()) {
      break;
    }
    const auto key = ReadJSONDocument(frame.key, heap);
    if (!key) {
      if (key.error().status == ExecutionStatus::OutOfMemory) {
        return std::unexpected(ExecutionStatus::OutOfMemory);
      }
      break;
    }
    const auto* value = key->root();
    AppendJSONKey(path, {yyjson_get_str(value), yyjson_get_len(value)});
  }
  return path;
}

}  // namespace vela::runtime
