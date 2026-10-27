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
#include <tuple>

#include "base/execution_status.h"
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
      : roots(heap, values.data(), values.size()) {
    Reset(parent, value, kind);
  }
  void Reset(JsonEncodeFrame* next_parent, Value value, JsonFrameKind next_kind) {
    parent = next_parent;
    values.front() = value;
    kind = next_kind;
    index = 0;
  }
  JsonEncodeFrame* parent = nullptr;
  std::array<Value, 2> values{};
  RootRange roots;
  JsonFrameKind kind = JsonFrameKind::Conversion;
  const ArrayLayout* elements = nullptr;
  CollectionIterator iterator;
  std::uint64_t mutation = 0;
  std::uint32_t index = 0;
};

JsonEncoder::JsonEncoder(Isolate& isolate, Value input)
    : heap_(&isolate.heap()),
      active_type_(isolate.CreateCollectionLayout({.keys = {.element = {.dynamic = true}}})),
      values_{input, Value::Null(), Value::Null()},
      roots_(*heap_, values_.data(), values_.size()) {}

JsonEncoder::~JsonEncoder() {
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

JsonRuntimeError JsonEncoder::Error(ExecutionStatus status, std::string_view message) const {
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
  return {.status = status, .message = std::string(message) + " at $" + path};
}

JsonRuntimeError JsonEncoder::ConversionError(std::string_view message) const {
  return Error(ExecutionStatus::JsonEncodeError, message);
}

std::expected<void, JsonRuntimeError> JsonEncoder::Append(std::string_view text) {
  if (!heap_->AppendString(*values_.at(kOutput).AsObject(), text)) {
    return std::unexpected(Error(ExecutionStatus::OutOfMemory, "JSON output allocation failed"));
  }
  return {};
}

std::expected<void, JsonRuntimeError> JsonEncoder::Quote(std::string_view text) {
  constexpr std::size_t kUnicodeEscapeLength = 6;
  if (text.size() > kMaximumStringLength - 2) {
    return std::unexpected(Error(ExecutionStatus::OutOfMemory, "JSON output allocation failed"));
  }
  auto length = text.size() + 2;
  for (const auto character : text) {
    const auto byte = static_cast<unsigned char>(character);
    std::size_t extra = 0;
    if (byte < ' ') {
      extra = kUnicodeEscapeLength - 1;
    } else if (byte == '"' || byte == '\\') {
      extra = 1;
    }
    if (extra > kMaximumStringLength - length) {
      return std::unexpected(Error(ExecutionStatus::OutOfMemory, "JSON output allocation failed"));
    }
    length += extra;
  }
  const auto storage = heap_->AppendStringStorage(*values_.at(kOutput).AsObject(), length);
  if (!storage) {
    return std::unexpected(Error(storage.error(), "JSON output allocation failed"));
  }
  auto output = *storage;
  output.front() = '"';
  output = output.subspan(1);
  constexpr std::string_view kHex = "0123456789abcdef";
  constexpr unsigned kHexDigitMask = 15;
  for (const auto character : text) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte >= ' ' && byte != '"' && byte != '\\') {
      output.front() = character;
      output = output.subspan(1);
      continue;
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
    std::ranges::copy(encoded, output.begin());
    output = output.subspan(encoded.size());
  }
  output.front() = '"';
  return {};
}

std::expected<void, JsonRuntimeError> JsonEncoder::Push(Value value,
                                                        JsonFrameKind kind,
                                                        const ArrayLayout* elements) {
  auto& active = *values_.at(kActive).AsObject();
  const auto inserted = heap_->InsertCollection(active, value);
  if (!inserted) {
    return std::unexpected(Error(inserted.error(), "JSON traversal allocation failed"));
  }
  if (*inserted == CollectionInsert::Updated) {
    return std::unexpected(Error(ExecutionStatus::JsonEncodeError, "cyclic JSON value"));
  }
  if (spare_ != nullptr) {
    auto* frame = spare_;
    spare_ = frame->parent;
    frame->Reset(frame_, value, kind);
    frame->roots.Attach(*heap_, frame->values.data(), frame->values.size());
    frame_ = frame;
  } else {
    auto* storage = static_cast<JsonEncodeFrame*>(heap_->AllocateNative(sizeof(JsonEncodeFrame)));
    if (storage == nullptr) {
      return std::unexpected(
          Error(ExecutionStatus::OutOfMemory, "JSON traversal allocation failed"));
    }
    frame_ = std::construct_at(storage, *heap_, frame_, value, kind);
  }
  frame_->elements = elements;
  auto& object = *value.AsObject();
  if (kind == JsonFrameKind::Array) {
    frame_->mutation = *Heap::ArrayMutation(object);
  } else if (kind == JsonFrameKind::Map) {
    frame_->iterator = *Heap::IterateCollection(object);
  }
  return {};
}

void JsonEncoder::Pop() {
  auto* frame = frame_;
  frame_ = frame->parent;
  std::ignore = Heap::RemoveCollection(*values_.at(kActive).AsObject(), frame->values.at(0));
  frame->roots.Detach();
  frame->values.fill(Value::Null());
  frame->iterator = {};
  frame->parent = spare_;
  spare_ = frame;
}

void JsonEncoder::ResumeConversion(Value result) {
  assert(frame_ != nullptr && frame_->kind == JsonFrameKind::Conversion && !pending_);
  values_.at(kPending) = result;
  pending_ = true;
}

std::expected<void, JsonRuntimeError> JsonEncoder::Initialize() {
  auto* builder = heap_->AllocateMutableString();
  if (builder == nullptr) {
    return std::unexpected(Error(ExecutionStatus::OutOfMemory, "JSON output allocation failed"));
  }
  values_.at(kOutput) = Value::Object(builder);
  auto* active = heap_->AllocateCollection(*active_type_);
  if (active == nullptr) {
    return std::unexpected(Error(ExecutionStatus::OutOfMemory, "JSON traversal allocation failed"));
  }
  values_.at(kActive) = Value::Object(active);
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
    return std::unexpected(Error(ExecutionStatus::JsonEncodeError, "non-finite JSON number"));
  }
  std::array<char, kScalarTextCapacity> buffer{};
  return Append(FormatJsonScalar(value, buffer));
}

std::expected<std::optional<Value>, JsonRuntimeError> JsonEncoder::EnterObject(Value value) {
  auto& object = *value.AsObject();
  const auto kind = Heap::KindOf(object);
  const ArrayLayout* elements = nullptr;
  auto frame_kind = JsonFrameKind::Conversion;
  switch (kind) {
    case ObjectKind::Array:
      frame_kind = JsonFrameKind::Array;
      elements = Heap::ArrayTypeOf(object);
      break;
    case ObjectKind::Collection: {
      const auto& values = Heap::CollectionTypeOf(object)->values;
      if (!values) {
        return std::unexpected(
            Error(ExecutionStatus::JsonEncodeError, "Set is not JSON encodable"));
      }
      frame_kind = JsonFrameKind::Map;
      elements = &*values;
      break;
    }
    case ObjectKind::Instance:
      if (Heap::ClassOf(object)->IsEnum()) {
        return std::unexpected(
            Error(ExecutionStatus::JsonEncodeError, "enum is not JSON encodable"));
      }
      break;
    default:
      return std::unexpected(Error(ExecutionStatus::JsonEncodeError, "unsupported JSON value"));
  }
  if (auto pushed = Push(value, frame_kind, elements); !pushed) {
    return std::unexpected(pushed.error());
  }
  if (kind == ObjectKind::Instance) {
    return value;
  }
  if (auto written = Append(kind == ObjectKind::Array ? "[" : "{"); !written) {
    return std::unexpected(written.error());
  }
  return std::nullopt;
}

std::expected<std::optional<Value>, JsonRuntimeError> JsonEncoder::EncodeValue() {
  const auto value = values_.at(kPending);
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
  if (instance != nullptr && instance->IsEnum() && !values_.at(kPending).IsNull()) {
    return std::unexpected(Error(ExecutionStatus::JsonEncodeError, "enum is not JSON encodable"));
  }
  return {};
}

std::expected<void, JsonRuntimeError> JsonEncoder::AdvanceArray() {
  auto& frame = *frame_;
  auto& object = *frame.values.front().AsObject();
  if (frame.mutation != *Heap::ArrayMutation(object)) {
    return std::unexpected(
        Error(ExecutionStatus::ConcurrentModification, "array changed during JSON conversion"));
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
  values_.at(kPending) = *Heap::GetElement(object, frame.index++);
  pending_ = true;
  return CheckElement(*frame.elements);
}

std::expected<void, JsonRuntimeError> JsonEncoder::AdvanceMap() {
  auto& frame = *frame_;
  const auto entry = frame.iterator.Next();
  if (!entry) {
    return std::unexpected(Error(entry.error(), "map changed during JSON conversion"));
  }
  if (!*entry) {
    if (auto written = Append("}"); !written) {
      return written;
    }
    Pop();
    return {};
  }
  frame.values.at(1) = (**entry).key;
  values_.at(kPending) = (**entry).value;
  if (auto checked = CheckElement(*frame.elements); !checked) {
    return checked;
  }
  if (!frame.values.at(1).IsString()) {
    return std::unexpected(
        Error(ExecutionStatus::JsonEncodeError, "JSON object key must be string"));
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

std::expected<Value, JsonRuntimeError> JsonEncoder::Advance() {
  if (values_.at(kOutput).IsNull()) {
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
      auto* output = heap_->BuildString(*values_.at(kOutput).AsObject());
      if (output == nullptr) {
        return std::unexpected(
            Error(ExecutionStatus::OutOfMemory, "JSON output allocation failed"));
      }
      return Value::String(output);
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
