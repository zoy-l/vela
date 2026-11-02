#include "runtime/native/native_json.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <absl/hash/hash.h>
#include <yyjson.h>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "base/json_decode.h"
#include "runtime/collections/array_operations.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/native_buffer.h"
#include "runtime/isolate/isolate.h"
#include "runtime/isolate/program_layouts.h"
#include "runtime/json/json.h"
#include "runtime/json/json_path.h"
#include "runtime/json/parsed_json.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_control.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/strings/string.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

constexpr auto kNoTask = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kErrorFieldCount = 6;

JsonRuntimeError Oom() {
  return {
      .status = ExecutionStatus::OutOfMemory,
      .message = "JSON decode allocation failed",
      .expected = {},
      .actual = {},
  };
}

std::string_view Text(const yyjson_val* value) {
  return {yyjson_get_str(value), yyjson_get_len(value)};
}

std::string_view Actual(const ParsedJSON& document, const yyjson_val* value) {
  if (value == nullptr) {
    return "missing";
  }
  if (yyjson_is_null(value)) {
    return "null";
  }
  if (yyjson_is_bool(value)) {
    return "bool";
  }
  if (yyjson_is_raw(value)) {
    return document.Number(value).IsInteger() ? "int" : "double";
  }
  if (yyjson_is_str(value)) {
    return "string";
  }
  return yyjson_is_arr(value) ? "array" : "object";
}

struct DecodeTask final {
  yyjson_val* value = nullptr;
  std::uint32_t plan = 0;
  std::size_t parent = kNoTask;
  std::size_t first_child = kNoTask;
  std::size_t last_child = kNoTask;
  std::size_t next = kNoTask;
  std::string_view key;
  std::size_t position = 0;
  bool array_element = false;
};

struct ParameterSlot final {
  std::size_t owner = kNoTask;
  std::string_view name;
};

// 校验先生成有限值树的后序次序；物化只消费该次序，用户构造调用不递归驱动 JSON 遍历。
class DecodeSession final {
 public:
  DecodeSession(NativeExecution& execution, ParsedJSON& document)
      : execution_(&execution),
        document_(&document),
        heap_(&execution.isolate().heap()),
        plans_(execution.instance().image().json_decode_plans.values()),
        tasks_(*heap_, document.size()),
        order_(*heap_, document.size()),
        values_(*heap_, document.size()),
        // 每个对象键至少对应 key/value 两个 token；只登记存在的参数，装载率低于一半。
        parameters_(*heap_, document.size()) {}

  std::expected<void, JsonRuntimeError> Validate(std::uint32_t plan);
  std::expected<Value, ExecutionStatus> Materialize();

 private:
  [[nodiscard]] std::uint32_t NonNullablePlan(std::uint32_t plan) const;
  [[nodiscard]] std::size_t ParameterIndex(std::size_t owner, std::string_view name) const;
  void AddChild(std::size_t parent,
                yyjson_val* value,
                std::uint32_t plan,
                std::string_view key,
                std::size_t position);
  std::expected<void, JsonRuntimeError> ValidateTask(std::size_t index);
  std::expected<void, JsonRuntimeError> ValidateObject(std::size_t index);
  std::expected<void, JsonRuntimeError> PopulateChildren(std::size_t index);
  JsonRuntimeError Error(std::size_t index,
                         JsonRuntimeError::Kind kind,
                         std::optional<std::string_view> key = std::nullopt,
                         yyjson_val* token = nullptr);
  std::string Path(std::size_t index);
  std::expected<Value, ExecutionStatus> MaterializeTask(std::size_t index);
  std::expected<Value, ExecutionStatus> MaterializeClass(std::size_t index);
  std::expected<Value, ExecutionStatus> MaterializeContainer(std::size_t index);

  NativeExecution* execution_;
  ParsedJSON* document_;
  Heap* heap_;
  std::span<const NativeJSONDecodePlan> plans_;
  NativeBuffer<DecodeTask> tasks_;
  NativeBuffer<std::size_t> order_;
  NativeBuffer<Value> values_;
  NativeBuffer<ParameterSlot> parameters_;
  RootRange roots_{RootOwner::Runtime};
  std::size_t count_ = 1;
  std::size_t completed_ = 0;
};

std::uint32_t DecodeSession::NonNullablePlan(std::uint32_t plan) const {
  while (plans_[plan].kind == JSONDecodeKind::Nullable) {
    plan = plans_[plan].element;
  }
  return plan;
}

std::size_t DecodeSession::ParameterIndex(std::size_t owner, std::string_view name) const {
  const auto slots = parameters_.values();
  auto slot = absl::HashOf(owner, name) % slots.size();
  while (slots[slot].owner != kNoTask && (slots[slot].owner != owner || slots[slot].name != name)) {
    slot = (slot + 1) % slots.size();
  }
  return slot;
}

void DecodeSession::AddChild(std::size_t parent,
                             yyjson_val* value,
                             std::uint32_t plan,
                             std::string_view key,
                             std::size_t position) {
  const auto index = count_++;
  const auto tasks = tasks_.values();
  auto& owner = tasks[parent];
  tasks[index] = {
      .value = value,
      .plan = plan,
      .parent = parent,
      .key = key,
      .position = position,
      .array_element = yyjson_is_arr(owner.value),
  };
  if (owner.last_child == kNoTask) {
    owner.first_child = index;
  } else {
    tasks[owner.last_child].next = index;
  }
  owner.last_child = index;
}

std::string DecodeSession::Path(std::size_t index) {
  const auto tasks = tasks_.values();
  const auto order = order_.values();
  std::size_t count = 0;
  while (tasks[index].parent != kNoTask) {
    order[count++] = index;
    index = tasks[index].parent;
  }
  std::string path = "$";
  while (count != 0) {
    const auto& task = tasks[order[--count]];
    if (task.array_element) {
      path += '[' + std::to_string(task.position) + ']';
    } else {
      AppendJSONKey(path, task.key);
    }
  }
  return path;
}

JsonRuntimeError DecodeSession::Error(std::size_t index,
                                      JsonRuntimeError::Kind kind,
                                      std::optional<std::string_view> key,
                                      yyjson_val* token) {
  const auto& task = tasks_.values()[index];
  const auto& plan = plans_[NonNullablePlan(task.plan)];
  auto path = Path(index);
  if (key) {
    AppendJSONKey(path, *key);
  }
  const auto* value = key ? token : task.value;
  const auto* actual_value = value;
  if (kind == JsonRuntimeError::Kind::UnknownField && key) {
    actual_value = document_->Field(task.value, *key);
  }
  std::optional<std::string> expected;
  if (kind == JsonRuntimeError::Kind::TypeMismatch) {
    expected = plans_[task.plan].name.text();
  } else if (kind == JsonRuntimeError::Kind::MissingField && key) {
    for (const auto& parameter : plan.parameters.values()) {
      if (parameter.name.text() == *key) {
        expected = plans_[parameter.node].name.text();
        break;
      }
    }
  }
  std::string message = "JSON value has the wrong type";
  if (kind == JsonRuntimeError::Kind::MissingField) {
    message = "required JSON field is missing";
  } else if (kind == JsonRuntimeError::Kind::UnknownField) {
    message = "unknown JSON field";
  }
  return {
      .offset = value == nullptr ? 0 : document_->Offset(value),
      .message = std::move(message),
      .kind = kind,
      .path = std::move(path),
      .expected = std::move(expected),
      .actual = std::string(Actual(*document_, actual_value)),
      .has_offset = value != nullptr,
  };
}

std::expected<void, JsonRuntimeError> DecodeSession::ValidateObject(std::size_t index) {
  const auto& task = tasks_.values()[index];
  const auto plan_index = NonNullablePlan(task.plan);
  const auto& plan = plans_[plan_index];
  for (const auto& parameter : plan.parameters.values()) {
    const auto name = parameter.name.text();
    const auto* value = document_->Field(task.value, name);
    if (parameter.optional == 0 && value == nullptr) {
      return std::unexpected(Error(index, JsonRuntimeError::Kind::MissingField, name));
    }
    if (value != nullptr) {
      parameters_.values()[ParameterIndex(index, name)] = {.owner = index, .name = name};
    }
  }
  auto iterator = yyjson_obj_iter_with(task.value);
  while (auto* key = yyjson_obj_iter_next(&iterator)) {
    if (document_->IsFirstKey(task.value, key) &&
        parameters_.values()[ParameterIndex(index, Text(key))].owner == kNoTask) {
      return std::unexpected(Error(index, JsonRuntimeError::Kind::UnknownField, Text(key), key));
    }
  }
  std::size_t position = 0;
  for (const auto& parameter : plan.parameters.values()) {
    if (auto* child = document_->Field(task.value, parameter.name.text())) {
      AddChild(index, child, parameter.node, parameter.name.text(), position);
    }
    ++position;
  }
  return {};
}

std::expected<void, JsonRuntimeError> DecodeSession::PopulateChildren(std::size_t index) {
  const auto& task = tasks_.values()[index];
  const auto plan_index = NonNullablePlan(task.plan);
  const auto& plan = plans_[plan_index];
  const auto child_plan = plan.kind == JSONDecodeKind::Dynamic ? plan_index : plan.element;
  if (plan.kind == JSONDecodeKind::Class) {
    return ValidateObject(index);
  }
  std::size_t position = 0;
  if (yyjson_is_arr(task.value)) {
    auto iterator = yyjson_arr_iter_with(task.value);
    while (auto* child = yyjson_arr_iter_next(&iterator)) {
      AddChild(index, child, child_plan, {}, position++);
    }
  } else if (yyjson_is_obj(task.value)) {
    auto iterator = yyjson_obj_iter_with(task.value);
    while (auto* key = yyjson_obj_iter_next(&iterator)) {
      if (document_->IsFirstKey(task.value, key)) {
        AddChild(index, document_->Field(task.value, Text(key)), child_plan, Text(key), position++);
      }
    }
  }
  return {};
}

std::expected<void, JsonRuntimeError> DecodeSession::ValidateTask(std::size_t index) {
  const auto& task = tasks_.values()[index];
  if (plans_[task.plan].kind == JSONDecodeKind::Nullable && yyjson_is_null(task.value)) {
    return {};
  }
  const auto kind = plans_[NonNullablePlan(task.plan)].kind;
  bool matches = false;
  switch (kind) {
    case JSONDecodeKind::Dynamic:
      matches = true;
      break;
    case JSONDecodeKind::Bool:
      matches = yyjson_is_bool(task.value);
      break;
    case JSONDecodeKind::Int:
      matches = yyjson_is_raw(task.value) && document_->Number(task.value).IsInteger();
      break;
    case JSONDecodeKind::Double:
      matches = yyjson_is_raw(task.value) && document_->Number(task.value).IsDouble();
      break;
    case JSONDecodeKind::String:
      matches = yyjson_is_str(task.value);
      break;
    case JSONDecodeKind::Array:
      matches = yyjson_is_arr(task.value);
      break;
    case JSONDecodeKind::Class:
    case JSONDecodeKind::Map:
      matches = yyjson_is_obj(task.value);
      break;
    case JSONDecodeKind::Nullable:
      break;
  }
  if (!matches) {
    return std::unexpected(Error(index, JsonRuntimeError::Kind::TypeMismatch));
  }
  return PopulateChildren(index);
}

std::expected<void, JsonRuntimeError> DecodeSession::Validate(std::uint32_t plan) {
  if (!tasks_.IsAllocated() || !order_.IsAllocated() || !values_.IsAllocated() ||
      !parameters_.IsAllocated()) {
    return std::unexpected(Oom());
  }
  const auto tasks = tasks_.values();
  const auto parameters = parameters_.values();
  std::uninitialized_value_construct_n(tasks.data(), tasks.size());
  std::uninitialized_value_construct_n(parameters.data(), parameters.size());
  std::uninitialized_value_construct_n(values_.values().data(), values_.values().size());
  roots_.Attach(*heap_, values_.values().data(), values_.values().size());
  tasks.front().value = document_->root();
  tasks.front().plan = plan;
  std::size_t index = 0;
  for (;;) {
    const auto status = execution_->isolate().native_control().Poll();
    if (status != ExecutionStatus::Returned) {
      return std::unexpected(JsonRuntimeError{
          .status = status,
          .message = "JSON decoding stopped",
          .expected = {},
          .actual = {},
      });
    }
    if (auto validated = ValidateTask(index); !validated) {
      return validated;
    }
    if (tasks[index].first_child != kNoTask) {
      index = tasks[index].first_child;
      continue;
    }
    for (;;) {
      order_.values()[completed_++] = index;
      if (tasks[index].next != kNoTask) {
        index = tasks[index].next;
        break;
      }
      index = tasks[index].parent;
      if (index == kNoTask) {
        return {};
      }
    }
  }
}

std::expected<Value, ExecutionStatus> DecodeSession::MaterializeClass(std::size_t index) {
  const auto& task = tasks_.values()[index];
  const auto& plan = plans_[NonNullablePlan(task.plan)];
  const auto count = plan.parameters.size;
  NativeBuffer<NativeValue> arguments(*heap_, count * 2);
  if (!arguments.IsAllocated()) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto input = arguments.values();
  for (std::size_t parameter = 0; parameter < count; ++parameter) {
    input[parameter] = EncodeNativeValue(Value::Null());
    input[count + parameter] = EncodeNativeValue(Value::Boolean(false));
  }
  for (auto child = task.first_child; child != kNoTask; child = tasks_.values()[child].next) {
    const auto parameter = tasks_.values()[child].position;
    input[parameter] = EncodeNativeValue(values_.values()[child]);
    input[count + parameter] = EncodeNativeValue(Value::Boolean(true));
  }
  return InvokeNativeTarget(*execution_, plan.constructor, input);
}

std::expected<Value, ExecutionStatus> DecodeSession::MaterializeTask(std::size_t index) {
  const auto& task = tasks_.values()[index];
  const auto& plan = plans_[NonNullablePlan(task.plan)];
  if (yyjson_is_null(task.value)) {
    return Value::Null();
  }
  if (yyjson_is_bool(task.value)) {
    return Value::Boolean(yyjson_get_bool(task.value));
  }
  if (yyjson_is_raw(task.value)) {
    return document_->Number(task.value);
  }
  if (yyjson_is_str(task.value)) {
    auto* string = heap_->AllocateString(Text(task.value));
    return string != nullptr ? std::expected<Value, ExecutionStatus>(Value::String(string))
                             : std::unexpected(ExecutionStatus::OutOfMemory);
  }
  if (plan.kind == JSONDecodeKind::Class) {
    return MaterializeClass(index);
  }
  return MaterializeContainer(index);
}

std::expected<Value, ExecutionStatus> DecodeSession::MaterializeContainer(std::size_t index) {
  const auto& task = tasks_.values()[index];
  const auto& plan = plans_[NonNullablePlan(task.plan)];
  auto& isolate = execution_->isolate();
  const auto& layouts = execution_->instance().layouts();
  HeapObject* object = nullptr;
  if (yyjson_is_arr(task.value)) {
    const auto* layout = plan.kind == JSONDecodeKind::Dynamic
                             ? isolate.CreateArrayLayout({.element = {.dynamic = true}})
                             : layouts.types.at(plan.type).array;
    object = heap_->AllocateArray(*layout, yyjson_get_len(task.value));
  } else {
    const auto* layout = plan.kind == JSONDecodeKind::Dynamic
                             ? isolate.CreateCollectionLayout({
                                   .keys = {.element = {.kind = ValueKind::String}},
                                   .values = ArrayLayout{.element = {.dynamic = true}},
                               })
                             : layouts.types.at(plan.type).collection;
    object = heap_->AllocateCollection(*layout);
  }
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  auto container = Value::Object(object);
  RootRange root(*heap_, &container, 1, RootOwner::Runtime);
  for (auto child = task.first_child; child != kNoTask; child = tasks_.values()[child].next) {
    const auto& entry = tasks_.values()[child];
    const auto value = values_.values()[child];
    if (entry.array_element) {
      if (auto stored =
              heap_->SetElement(*object, static_cast<std::uint32_t>(entry.position), value);
          !stored) {
        return std::unexpected(ArrayExecutionStatus(stored.error()));
      }
    } else {
      auto* string = heap_->AllocateString(entry.key);
      if (string == nullptr) {
        return std::unexpected(ExecutionStatus::OutOfMemory);
      }
      auto key = Value::String(string);
      RootRange key_root(*heap_, &key, 1, RootOwner::Runtime);
      if (auto stored = heap_->InsertCollection(*object, key, value); !stored) {
        return std::unexpected(stored.error());
      }
    }
  }
  return container;
}

std::expected<Value, ExecutionStatus> DecodeSession::Materialize() {
  for (const auto index : order_.values().first(completed_)) {
    const auto status = execution_->isolate().native_control().Poll();
    if (status != ExecutionStatus::Returned) {
      return std::unexpected(status);
    }
    auto value = MaterializeTask(index);
    if (!value) {
      return value;
    }
    values_.values()[index] = *value;
  }
  return values_.values().front();
}

std::string_view ErrorCode(JsonRuntimeError::Kind kind) {
  switch (kind) {
    case JsonRuntimeError::Kind::Syntax:
      return "syntax_error";
    case JsonRuntimeError::Kind::NumberOutOfRange:
      return "number_out_of_range";
    case JsonRuntimeError::Kind::TypeMismatch:
      return "type_mismatch";
    case JsonRuntimeError::Kind::MissingField:
      return "missing_field";
    case JsonRuntimeError::Kind::UnknownField:
      return "unknown_field";
  }
  std::unreachable();
}

std::expected<Value, ExecutionStatus> Record(NativeExecution& execution,
                                             std::uint32_t type,
                                             std::span<Value> fields) {
  auto& heap = execution.isolate().heap();
  RootRange roots(heap, fields.data(), fields.size(), RootOwner::Runtime);
  auto* object = heap.AllocateInstance(*execution.instance().layouts().classes.at(type));
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  auto result = Value::Object(object);
  RootRange record(heap, &result, 1, RootOwner::Runtime);
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (auto stored = heap.SetField(*object, static_cast<std::uint32_t>(index), fields[index]);
        !stored) {
      return std::unexpected(stored.error());
    }
  }
  return result;
}

std::expected<Value, ExecutionStatus> InputError(NativeExecution& execution,
                                                 std::uint32_t type,
                                                 const JsonRuntimeError& error) {
  auto& heap = execution.isolate().heap();
  std::array<Value, kErrorFieldCount> fields;
  RootRange roots(heap, fields.data(), fields.size(), RootOwner::Runtime);
  const std::array<std::optional<std::string_view>, kErrorFieldCount> texts{
      ErrorCode(error.kind), error.path, error.expected, error.actual, std::nullopt, error.message,
  };
  for (std::size_t index = 0; index < texts.size(); ++index) {
    const auto& text = texts[index];
    if (text) {
      auto* string = heap.AllocateString(*text);
      if (string == nullptr) {
        return std::unexpected(ExecutionStatus::OutOfMemory);
      }
      fields[index] = Value::String(string);
    }
  }
  if (error.has_offset) {
    fields[4] = Value::Integer(static_cast<std::int64_t>(error.offset));
  }
  return Record(execution, type, fields);
}

std::expected<Value, ExecutionStatus> Failure(NativeExecution& execution,
                                              Builtin builtin,
                                              std::span<const Value> arguments,
                                              const JsonRuntimeError& error) {
  if (error.status != ExecutionStatus::JsonDecodeError) {
    return std::unexpected(error.status);
  }
  auto payload = InputError(execution, static_cast<std::uint32_t>(arguments[2].AsInteger()), error);
  if (!payload) {
    return payload;
  }
  if (builtin == Builtin::JsonTypedParse) {
    return std::unexpected(execution.Throw(*payload));
  }
  std::array fields{Value::Boolean(false), *payload};
  return Record(execution, static_cast<std::uint32_t>(arguments[3].AsInteger()), fields);
}

}  // namespace

std::expected<Value, ExecutionStatus> ExecuteNativeTypedJSON(NativeExecution& execution,
                                                             Builtin builtin,
                                                             std::span<const Value> arguments) {
  const auto status = execution.isolate().native_control().Poll();
  if (status != ExecutionStatus::Returned) {
    return std::unexpected(status);
  }
  auto source = arguments.front();
  RootRange root(execution.isolate().heap(), &source, 1, RootOwner::Runtime);
  auto document = ParsedJSON::Parse(source.AsString()->value(), execution.isolate().heap());
  if (!document) {
    return Failure(execution, builtin, arguments, document.error());
  }
  DecodeSession session(execution, **document);
  if (auto validated = session.Validate(static_cast<std::uint32_t>(arguments[1].AsInteger()));
      !validated) {
    return Failure(execution, builtin, arguments, validated.error());
  }
  auto value = session.Materialize();
  if (!value || builtin == Builtin::JsonTypedParse) {
    return value;
  }
  std::array fields{Value::Boolean(true), *value};
  return Record(execution, static_cast<std::uint32_t>(arguments[3].AsInteger()), fields);
}

}  // namespace vela::runtime
