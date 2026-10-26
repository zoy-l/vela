#include "runtime/json.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "runtime/value_io.h"

namespace vela::runtime {

enum class JsonFrameKind : std::uint8_t { Array, Map, Conversion };

struct JsonEncodeFrame final {
  JsonEncodeFrame(Heap& heap, JsonEncodeFrame* parent, Value value, JsonFrameKind kind)
      : parent(parent),
        values{value, Value::Null()},
        roots(heap, values.data(), values.size()),
        kind(kind) {}
  JsonEncodeFrame* parent;
  std::array<Value, 2> values;
  RootRange roots;
  JsonFrameKind kind;
  CollectionCursor cursor;
  std::uint64_t mutation = 0;
  std::uint32_t index = 0;
};

JsonEncoder::JsonEncoder(Isolate& isolate, Value input)
    : heap_(&isolate.heap()),
      active_type_(isolate.CreateCollectionLayout({.keys = {.element = {.dynamic = true}}})),
      values_{input, Value::Null(), Value::Null()},
      roots_(*heap_, values_.data(), values_.size()) {}

JsonEncoderPtr JsonEncoder::Create(Isolate& isolate, Value input) {
  auto* storage = static_cast<JsonEncoder*>(isolate.heap().AllocateNative(sizeof(JsonEncoder)));
  return JsonEncoderPtr(storage == nullptr ? nullptr : std::construct_at(storage, isolate, input));
}

void JsonEncoderDeleter::operator()(JsonEncoder* encoder) const {
  auto* heap = encoder->heap_;
  std::destroy_at(encoder);
  heap->FreeNative(encoder);
}

JsonEncoder::~JsonEncoder() {
  while (frame_ != nullptr) {
    Pop();
  }
}

JsonRuntimeError JsonEncoder::Error(JsonFailure failure, std::string_view message) const {
  std::string path;
  // 失败时从叶到根反向写入，最后翻转；避免深层错误路径反复前插产生平方复杂度。
  const auto append_reversed = [&path](std::string_view text) {
    path.append(text.rbegin(), text.rend());
  };
  for (auto* frame = frame_; frame != nullptr; frame = frame->parent) {
    if (frame->kind == JsonFrameKind::Array && frame->index != 0) {
      path += ']';
      append_reversed(std::to_string(frame->index - 1));
      path += '[';
    } else if (frame->kind == JsonFrameKind::Map && frame->values.at(1).IsString()) {
      const auto key = frame->values.at(1).AsString()->value();
      path += "]\"";
      append_reversed(key);
      path += "\"[";
    }
  }
  std::ranges::reverse(path);
  return {.failure = failure, .message = std::string(message) + " at $" + path};
}

JsonRuntimeError JsonEncoder::ConversionError(std::string_view message) const {
  return Error(JsonFailure::Encode, message);
}

std::expected<void, JsonRuntimeError> JsonEncoder::Append(std::string_view text) {
  if (!heap_->AppendString(*values_.at(1).AsObject(), text)) {
    return std::unexpected(Error(JsonFailure::OutOfMemory, "JSON output allocation failed"));
  }
  return {};
}

std::expected<void, JsonRuntimeError> JsonEncoder::Quote(std::string_view text) {
  if (auto written = Append("\""); !written) {
    return written;
  }
  constexpr std::string_view kHex = "0123456789abcdef";
  constexpr std::size_t kUnicodeEscapeLength = 6;
  constexpr unsigned kHexDigitMask = 15;
  std::size_t start = 0;
  for (std::size_t index = 0; index < text.size(); ++index) {
    const auto byte = static_cast<unsigned char>(text.substr(index, 1).front());
    if (byte >= ' ' && byte != '"' && byte != '\\') {
      continue;
    }
    if (auto written = Append(text.substr(start, index - start)); !written) {
      return written;
    }
    std::array<char, kUnicodeEscapeLength> escape{
        '\\',
        'u',
        '0',
        '0',
        kHex.substr(byte >> 4U, 1).front(),
        kHex.substr(byte & kHexDigitMask, 1).front(),
    };
    std::string_view encoded(escape.data(), escape.size());
    if (byte == '"' || byte == '\\') {
      escape.at(1) = static_cast<char>(byte);
      encoded = encoded.substr(0, 2);
    }
    if (auto written = Append(encoded); !written) {
      return written;
    }
    start = index + 1;
  }
  if (auto written = Append(text.substr(start)); !written) {
    return written;
  }
  return Append("\"");
}

std::expected<void, JsonRuntimeError> JsonEncoder::Push(Value value) {
  auto& active = *values_.at(2).AsObject();
  const auto inserted = heap_->InsertCollection(active, value);
  if (!inserted) {
    return std::unexpected(Error(JsonFailure::OutOfMemory, "JSON traversal allocation failed"));
  }
  if (*inserted == CollectionInsert::Updated) {
    return std::unexpected(Error(JsonFailure::Encode, "cyclic JSON value"));
  }
  const auto object_kind = Heap::KindOf(*value.AsObject());
  auto kind = JsonFrameKind::Conversion;
  if (object_kind == ObjectKind::Array) {
    kind = JsonFrameKind::Array;
  } else if (object_kind == ObjectKind::Collection) {
    kind = JsonFrameKind::Map;
  }
  auto* storage = static_cast<JsonEncodeFrame*>(heap_->AllocateNative(sizeof(JsonEncodeFrame)));
  if (storage == nullptr) {
    return std::unexpected(Error(JsonFailure::OutOfMemory, "JSON traversal allocation failed"));
  }
  frame_ = std::construct_at(storage, *heap_, frame_, value, kind);
  auto& object = *value.AsObject();
  if (kind == JsonFrameKind::Array) {
    frame_->mutation = *Heap::ArrayMutation(object);
  } else if (kind == JsonFrameKind::Map) {
    frame_->cursor = *Heap::IterateCollection(object);
  }
  return {};
}

void JsonEncoder::Pop() {
  auto* frame = frame_;
  frame_ = frame->parent;
  const auto removed = Heap::RemoveCollection(*values_.at(2).AsObject(), frame->values.at(0));
  assert(removed && *removed == CollectionRemove::Removed);
  static_cast<void>(removed);
  std::destroy_at(frame);
  heap_->FreeNative(frame);
}

void JsonEncoder::ResumeConversion(Value result) {
  assert(frame_ != nullptr && frame_->kind == JsonFrameKind::Conversion && !pending_);
  values_.at(0) = result;
  pending_ = true;
}

std::expected<void, JsonRuntimeError> JsonEncoder::Initialize() {
  auto* builder = heap_->AllocateMutableString();
  if (builder == nullptr) {
    return std::unexpected(Error(JsonFailure::OutOfMemory, "JSON output allocation failed"));
  }
  values_.at(1) = Value::Object(builder);
  auto* active = heap_->AllocateCollection(*active_type_);
  if (active == nullptr) {
    return std::unexpected(Error(JsonFailure::OutOfMemory, "JSON traversal allocation failed"));
  }
  values_.at(2) = Value::Object(active);
  return {};
}

std::expected<void, JsonRuntimeError> JsonEncoder::EncodeScalar(Value value) {
  if (value.IsNull()) {
    return Append("null");
  }
  if (value.IsString()) {
    return Quote(value.AsString()->value());
  }
  if (value.IsDouble() && !std::isfinite(value.AsDouble())) {
    return std::unexpected(Error(JsonFailure::Encode, "non-finite JSON number"));
  }
  std::array<char, kScalarTextCapacity> buffer{};
  const auto text = FormatScalar(value, buffer);
  if (auto written = Append(text); !written) {
    return written;
  }
  return value.IsDouble() && text.find_first_of(".eE") == std::string_view::npos
             ? Append(".0")
             : std::expected<void, JsonRuntimeError>{};
}

std::expected<std::optional<JsonEncodeStep>, JsonRuntimeError> JsonEncoder::EnterObject(
    Value value) {
  auto& object = *value.AsObject();
  const auto kind = Heap::KindOf(object);
  const auto* layout = Heap::ClassOf(object);
  if (layout != nullptr && layout->IsEnum()) {
    return std::unexpected(Error(JsonFailure::Encode, "enum is not JSON encodable"));
  }
  if (kind == ObjectKind::Collection && !Heap::CollectionTypeOf(object)->values) {
    return std::unexpected(Error(JsonFailure::Encode, "Set is not JSON encodable"));
  }
  if (kind != ObjectKind::Array && kind != ObjectKind::Collection && kind != ObjectKind::Instance) {
    return std::unexpected(Error(JsonFailure::Encode, "unsupported JSON value"));
  }
  if (auto pushed = Push(value); !pushed) {
    return std::unexpected(pushed.error());
  }
  if (kind == ObjectKind::Instance) {
    return JsonEncodeStep{.action = JsonEncodeAction::Convert, .value = value};
  }
  if (auto written = Append(kind == ObjectKind::Array ? "[" : "{"); !written) {
    return std::unexpected(written.error());
  }
  return std::nullopt;
}

std::expected<std::optional<JsonEncodeStep>, JsonRuntimeError> JsonEncoder::EncodeValue() {
  const auto value = values_.front();
  if (value.IsObject()) {
    return EnterObject(value);
  }
  if (auto written = EncodeScalar(value); !written) {
    return std::unexpected(written.error());
  }
  return std::nullopt;
}

std::expected<void, JsonRuntimeError> JsonEncoder::CheckElement(const ArrayLayout& layout) const {
  const auto* instance = layout.element.instance;
  if (instance != nullptr && instance->IsEnum() && !values_.front().IsNull()) {
    return std::unexpected(Error(JsonFailure::Encode, "enum is not JSON encodable"));
  }
  return {};
}

std::expected<void, JsonRuntimeError> JsonEncoder::AdvanceArray() {
  auto& frame = *frame_;
  auto& object = *frame.values.front().AsObject();
  if (frame.mutation != *Heap::ArrayMutation(object)) {
    return std::unexpected(
        Error(JsonFailure::ConcurrentModification, "array changed during JSON conversion"));
  }
  if (frame.index == *Heap::ArrayLength(object)) {
    if (auto written = Append("]"); !written) {
      return written;
    }
    Pop();
    return {};
  }
  if (frame.index != 0) {
    if (auto written = Append(","); !written) {
      return written;
    }
  }
  values_.front() = *Heap::GetElement(object, frame.index++);
  pending_ = true;
  return CheckElement(*Heap::ArrayTypeOf(object));
}

std::expected<void, JsonRuntimeError> JsonEncoder::AdvanceMap() {
  auto& frame = *frame_;
  auto& object = *frame.values.front().AsObject();
  const auto entry = Heap::NextCollectionEntry(object, frame.cursor);
  if (!entry) {
    return std::unexpected(
        Error(JsonFailure::ConcurrentModification, "map changed during JSON conversion"));
  }
  if (!*entry) {
    if (auto written = Append("}"); !written) {
      return written;
    }
    Pop();
    return {};
  }
  frame.values.at(1) = (**entry).key;
  values_.front() = (**entry).value;
  if (auto checked = CheckElement(*Heap::CollectionTypeOf(object)->values); !checked) {
    return checked;
  }
  if (!frame.values.at(1).IsString()) {
    return std::unexpected(Error(JsonFailure::Encode, "JSON object key must be string"));
  }
  if (frame.index++ != 0) {
    if (auto written = Append(","); !written) {
      return written;
    }
  }
  if (auto written = Quote(frame.values.at(1).AsString()->value()); !written) {
    return written;
  }
  pending_ = true;
  return Append(":");
}

std::expected<JsonEncodeStep, JsonRuntimeError> JsonEncoder::Advance() {
  if (values_.at(1).IsNull()) {
    if (auto initialized = Initialize(); !initialized) {
      return std::unexpected(initialized.error());
    }
  }
  for (;;) {
    if (pending_) {
      pending_ = false;
      const auto step = EncodeValue();
      if (!step) {
        return std::unexpected(step.error());
      }
      if (*step) {
        return **step;
      }
      continue;
    }
    if (frame_ == nullptr) {
      auto* output = heap_->BuildString(*values_.at(1).AsObject());
      if (output == nullptr) {
        return std::unexpected(Error(JsonFailure::OutOfMemory, "JSON output allocation failed"));
      }
      return JsonEncodeStep{.action = JsonEncodeAction::Complete, .value = Value::String(output)};
    }
    if (frame_->kind == JsonFrameKind::Conversion) {
      Pop();
      continue;
    }
    const auto advanced = frame_->kind == JsonFrameKind::Array ? AdvanceArray() : AdvanceMap();
    if (!advanced) {
      return std::unexpected(advanced.error());
    }
  }
}

}  // namespace vela::runtime
