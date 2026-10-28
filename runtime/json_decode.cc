#include "runtime/json.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include <absl/strings/numbers.h>
#include <yyjson.h>

#include "base/execution_status.h"
#include "base/json.h"
#include "runtime/error.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

JsonRuntimeError OutOfMemory() {
  return {.status = ExecutionStatus::OutOfMemory, .message = "JSON allocation failed"};
}

void* Allocate(void* context, std::size_t size) {
  return static_cast<Heap*>(context)->AllocateNative(size);
}

void Free(void* context, void* pointer) {
  static_cast<Heap*>(context)->FreeNative(pointer);
}

void* Reallocate(void* context, void* pointer, std::size_t old_size, std::size_t size) {
  auto* replacement = Allocate(context, size);
  if (replacement != nullptr) {
    if (old_size != 0) {
      std::memcpy(replacement, pointer, old_size < size ? old_size : size);
    }
    Free(context, pointer);
  }
  return replacement;
}

std::expected<Value, JsonRuntimeError> Number(std::string_view token) {
  if (token.find_first_of(".eE") == std::string_view::npos) {
    std::int64_t integer = 0;
    const auto parsed =
        std::from_chars(std::to_address(token.begin()), std::to_address(token.end()), integer);
    if (parsed.ec == std::errc()) {
      return Value::Integer(integer);
    }
    return std::unexpected(JsonRuntimeError{.message = "JSON integer exceeds signed 64-bit range"});
  }
  double number = 0;
  if (!absl::SimpleAtod(token, &number) || !std::isfinite(number)) {
    return std::unexpected(JsonRuntimeError{.message = "JSON number exceeds finite double range"});
  }
  return Value::Double(number);
}

struct DecodeFrame final {
  DecodeFrame(Heap& heap, DecodeFrame* parent, yyjson_val* node, Value value)
      : roots(heap, &this->value, 1), key_roots(heap, &key, 1) {
    Reset(parent, node, value);
  }
  void Reset(DecodeFrame* next_parent, yyjson_val* node, Value next_value) {
    parent = next_parent;
    value = next_value;
    object = yyjson_is_obj(node);
    index = 0;
    if (object) {
      map = yyjson_obj_iter_with(node);
    } else {
      array = yyjson_arr_iter_with(node);
    }
  }
  DecodeFrame* parent = nullptr;
  // key 在分配子值时保持存活，根区间不跨越不同的 C++ 成员。
  Value value;
  Value key;
  RootRange roots;
  RootRange key_roots;
  bool object = false;
  yyjson_obj_iter map{};
  yyjson_arr_iter array{};
  std::uint32_t index = 0;
};

class Decoder final {
 public:
  Decoder(Isolate& isolate, const JsonDocument& document)
      : heap_(&isolate.heap()),
        document_(document),
        array_(isolate.CreateArrayLayout({.element = {.dynamic = true}})),
        map_(isolate.CreateCollectionLayout({
            .keys = {.element = {.kind = ValueKind::String}},
            .values = ArrayLayout{.element = {.dynamic = true}},
        })) {}
  ~Decoder() {
    while (frame_ != nullptr) {
      Pop();
    }
    while (spare_ != nullptr) {
      auto* frame = spare_;
      spare_ = frame->parent;
      std::destroy_at(frame);
      heap_->FreeNative(frame);
    }
  }
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
  Decoder(Decoder&&) = delete;
  Decoder& operator=(Decoder&&) = delete;
  std::expected<Value, JsonRuntimeError> Decode(yyjson_val* node) {
    auto root = Convert(node);
    if (!root) {
      return root;
    }
    RootRange roots(*heap_, &*root, 1);
    if (auto pushed = Push(node, *root); !pushed) {
      return std::unexpected(pushed.error());
    }
    while (frame_ != nullptr) {
      auto& frame = *frame_;
      yyjson_val* child = nullptr;
      if (frame.object) {
        auto* key = yyjson_obj_iter_next(&frame.map);
        if (key != nullptr) {
          auto* string = heap_->AllocateString({yyjson_get_str(key), yyjson_get_len(key)});
          if (string == nullptr) {
            return std::unexpected(OutOfMemory());
          }
          frame.key = Value::String(string);
          child = yyjson_obj_iter_get_val(key);
        }
      } else {
        child = yyjson_arr_iter_next(&frame.array);
      }
      if (child == nullptr) {
        Pop();
        continue;
      }
      auto value = Convert(child);
      if (!value) {
        return value;
      }
      RootRange child_root(*heap_, &*value, 1);
      auto& container = *frame.value.AsObject();
      if (frame.object) {
        if (!heap_->InsertCollection(container, frame.key, *value)) {
          return std::unexpected(OutOfMemory());
        }
      } else if (!heap_->SetElement(container, frame.index++, *value)) {
        return std::unexpected(OutOfMemory());
      }
      if (auto pushed = Push(child, *value); !pushed) {
        return std::unexpected(pushed.error());
      }
    }
    return root;
  }

 private:
  std::expected<void, JsonRuntimeError> Push(yyjson_val* node, Value value) {
    if (!yyjson_is_ctn(node)) {
      return {};
    }
    if (spare_ != nullptr) {
      auto* frame = spare_;
      spare_ = frame->parent;
      frame->Reset(frame_, node, value);
      frame->roots.Attach(*heap_, &frame->value, 1);
      frame->key_roots.Attach(*heap_, &frame->key, 1);
      frame_ = frame;
      return {};
    }
    auto* storage = static_cast<DecodeFrame*>(heap_->AllocateNative(sizeof(DecodeFrame)));
    if (storage == nullptr) {
      return std::unexpected(OutOfMemory());
    }
    frame_ = std::construct_at(storage, *heap_, frame_, node, value);
    return {};
  }
  void Pop() {
    auto* frame = frame_;
    frame_ = frame->parent;
    // 保留稳定地址供同层后续容器复用；空闲帧不保根已完成的值。
    frame->roots.Detach();
    frame->key_roots.Detach();
    frame->value = Value::Null();
    frame->key = Value::Null();
    frame->parent = spare_;
    spare_ = frame;
  }
  std::expected<Value, JsonRuntimeError> Convert(yyjson_val* node) {
    if (yyjson_is_null(node)) {
      return Value::Null();
    }
    if (yyjson_is_bool(node)) {
      return Value::Boolean(yyjson_get_bool(node));
    }
    if (yyjson_is_raw(node)) {
      const std::string_view token(yyjson_get_raw(node), yyjson_get_len(node));
      auto number = Number(token);
      if (!number) {
        number.error().offset = document_.RawOffset(*node);
      }
      return number;
    }
    if (yyjson_is_str(node)) {
      auto* string = heap_->AllocateString({yyjson_get_str(node), yyjson_get_len(node)});
      return string != nullptr ? std::expected<Value, JsonRuntimeError>(Value::String(string))
                               : std::unexpected(OutOfMemory());
    }
    auto* object = yyjson_is_arr(node) ? heap_->AllocateArray(*array_, yyjson_get_len(node))
                                       : heap_->AllocateCollection(*map_);
    return object != nullptr ? std::expected<Value, JsonRuntimeError>(Value::Object(object))
                             : std::unexpected(OutOfMemory());
  }
  Heap* heap_;
  const JsonDocument& document_;
  const ArrayLayout* array_;
  const CollectionLayout* map_;
  DecodeFrame* frame_ = nullptr;
  DecodeFrame* spare_ = nullptr;
};

}  // namespace

std::expected<Value, JsonRuntimeError> ParseJson(std::string_view source, Isolate& isolate) {
  const yyjson_alc allocator{
      .malloc = Allocate,
      .realloc = Reallocate,
      .free = Free,
      .ctx = &isolate.heap(),
  };
  auto document = JsonDocument::Parse(source, YYJSON_READ_NUMBER_AS_RAW, &allocator);
  if (!document) {
    const auto& error = document.error();
    return std::unexpected(JsonRuntimeError{
        .status =
            error.out_of_memory ? ExecutionStatus::OutOfMemory : ExecutionStatus::JsonDecodeError,
        .offset = error.offset,
        .message = error.message,
    });
  }
  return Decoder(isolate, *document).Decode(document->root());
}

std::expected<Value, ExecutionStatus> CreateJsonError(Isolate& isolate,
                                                      const JsonRuntimeError& error) {
  if (error.status != ExecutionStatus::JsonDecodeError &&
      error.status != ExecutionStatus::JsonEncodeError) {
    return std::unexpected(error.status);
  }
  const auto message = error.status == ExecutionStatus::JsonDecodeError
                           ? error.message + " at byte " + std::to_string(error.offset)
                           : error.message;
  return CreateError(isolate.heap(), isolate.error_layout(),
                     {.message = message, .code = ExecutionStatusCode(error.status)});
}

}  // namespace vela::runtime
