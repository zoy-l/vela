#include "runtime/native_instance.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/callable.h"
#include "base/execution_status.h"
#include "base/layout_description.h"
#include "base/member.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/layout_transaction.h"
#include "runtime/native_abi.h"
#include "runtime/native_build_identity.h"
#include "runtime/native_function.h"
#include "runtime/native_image.h"
#include "runtime/program_layouts.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

template <class Target, class Source, class Convert>
std::vector<Target> ConvertTable(NativeTable<Source> table, const Convert& convert) {
  std::vector<Target> result;
  result.reserve(table.size);
  std::ranges::transform(table.values(), std::back_inserter(result), convert);
  return result;
}

template <class T>
std::vector<T> CopyTable(NativeTable<T> table) {
  const auto values = table.values();
  return {values.begin(), values.end()};
}

std::optional<std::uint32_t> OptionalIndex(std::uint32_t index) {
  return index == kNoNativeIndex ? std::nullopt : std::optional(index);
}

std::string Text(NativeText text) {
  return std::string(text.text());
}

layout::Type DescribeType(const NativeTypeDescription& type) {
  return {
      .kind = static_cast<layout::Type::Kind>(type.kind),
      .nullable = type.nullable != 0,
      .index = type.index,
  };
}

layout::Class DescribeClass(const NativeClassDescription& source) {
  layout::Class result{
      .name = Text(source.name),
      .kind = static_cast<layout::Class::Kind>(source.kind),
      .field_count = source.field_count,
      .reference_fields = CopyTable(source.reference_fields),
      .callable = std::nullopt,
      .interfaces = CopyTable(source.interfaces),
      .dispatch_targets = CopyTable(source.dispatch_targets),
      .callable_type = OptionalIndex(source.callable_type),
      .expected_type = OptionalIndex(source.expected_type),
  };
  if (source.callable_function != kNoNativeIndex) {
    result.callable = CallableTarget{
        .function = source.callable_function,
        .mode = static_cast<CallableMode>(source.callable_mode),
    };
  }
  return result;
}

layout::Signature DescribeSignature(const NativeSignatureDescription& source) {
  layout::Signature result{
      .name = Text(source.name),
      .parameters = ConvertTable<layout::Signature::Parameter>(
          source.parameters,
          [](const NativeSignatureParameter& parameter) {
            return layout::Signature::Parameter{
                .type = DescribeType(parameter.type),
                .optional = (parameter.flags & kNativeOptionalParameter) != 0,
                .rest = (parameter.flags & kNativeRestParameter) != 0,
            };
          }),
  };
  if (source.has_result != 0) {
    result.result = DescribeType(source.result);
  }
  return result;
}

layout::Program DescribeLayouts(const NativeImage& image) {
  return {
      .interfaces = ConvertTable<std::string>(image.interfaces, Text),
      .classes = ConvertTable<layout::Class>(image.classes, DescribeClass),
      .arrays = ConvertTable<layout::Array>(image.arrays,
                                            [](const NativeArrayDescription& source) {
                                              return layout::Array{
                                                  .name = Text(source.name),
                                                  .element = DescribeType(source.element),
                                              };
                                            }),
      .collections = ConvertTable<layout::Collection>(
          image.collections,
          [](const NativeCollectionDescription& source) {
            layout::Collection result{.name = Text(source.name), .key = DescribeType(source.key)};
            if (source.has_value != 0) {
              result.value = DescribeType(source.value);
            }
            return result;
          }),
      .signatures = ConvertTable<layout::Signature>(image.signatures, DescribeSignature),
      .types = ConvertTable<layout::Type>(image.types, DescribeType),
      .array_types = CopyTable(image.array_types),
      .collection_types = CopyTable(image.collection_types),
  };
}

CallSignature DescribeCall(const NativeFunctionDescription& source) {
  CallSignature result{
      .parameters = ConvertTable<CallSignature::Parameter>(
          source.parameters,
          [](const NativeCallParameter& parameter) {
            return CallSignature::Parameter{
                .name = Text(parameter.name),
                .type = parameter.type,
                .optional = (parameter.flags & kNativeOptionalParameter) != 0,
                .rest = (parameter.flags & kNativeRestParameter) != 0,
            };
          }),
      .result = OptionalIndex(source.result_type),
      .hidden_parameters = static_cast<std::uint8_t>(source.hidden_parameters),
      .parameter_name_order = {},
  };
  result.IndexParameters();
  return result;
}

}  // namespace

std::string_view NativeRuntimeAbiIdentity() noexcept {
  return kNativeBuildIdentity;
}

std::string_view NativeTargetIdentity() noexcept {
  return kNativeTargetIdentity;
}

NativeInstance::NativeInstance(NativeProgramState& data, Value anchor)
    : data_(&data), anchor_(anchor), anchor_root_(data.isolate->heap(), &anchor_, 1) {}

NativeInstance NativeInstance::Borrow(HeapObject& state) {
  return {Heap::NativeProgram(state), Value::Object(&state)};
}

Value NativeInstance::constant(std::size_t index) const noexcept {
  return Heap::ValueSlots(*data_->values.at(NativeProgramState::kConstants).AsObject())
      .subspan(index)
      .front();
}

std::expected<Value, ExecutionStatus> NativeInstance::LoadGlobal(NativeExecution& execution,
                                                                 std::size_t index) {
  auto& state = data_->global_states.at(index);
  if (state == NativeProgramState::GlobalState::Ready) {
    return Heap::ValueSlots(*data_->values.at(NativeProgramState::kGlobals).AsObject())
        .subspan(index)
        .front();
  }
  if (state == NativeProgramState::GlobalState::Initializing) {
    return std::unexpected(ExecutionStatus::CyclicInitialization);
  }
  state = NativeProgramState::GlobalState::Initializing;
  const auto function = data_->image->global_initializers.values().subspan(index).front();
  const auto entry = data_->image->functions.values().subspan(function).front().entry;
  NativeValue result;
  const auto status = static_cast<ExecutionStatus>(entry(&execution, nullptr, 0, &result));
  if (status != ExecutionStatus::Returned) {
    state = NativeProgramState::GlobalState::Uninitialized;
    return std::unexpected(status);
  }
  const auto value = DecodeNativeValue(result);
  StoreGlobal(index, value);
  return value;
}

void NativeInstance::StoreGlobal(std::size_t index, Value value) noexcept {
  data_->isolate->heap().SetValueSlot(*data_->values.at(NativeProgramState::kGlobals).AsObject(),
                                      index, value);
  data_->global_states.at(index) = NativeProgramState::GlobalState::Ready;
}

std::expected<void, NativeInstantiationError> NativeInstance::BindImports(
    const layout::Program& description) {
  const auto functions = data_->image->functions.values();
  data_->signatures.reserve(functions.size());
  data_->imports.resize(functions.size());
  for (std::size_t index = 0; index < functions.size(); ++index) {
    const auto& function = functions.subspan(index).front();
    data_->signatures.push_back(DescribeCall(function));
    const auto symbol = function.native_symbol.text();
    if (symbol.empty()) {
      continue;
    }
    auto binding = data_->isolate->FindNative(symbol);
    if (binding == nullptr) {
      return std::unexpected(NativeInstantiationError{
          .status = NativeInstantiationStatus::MissingImport,
          .message = "unregistered native function '" + std::string(symbol) + "'",
      });
    }
    if (!binding->IsSignatureCompatible(data_->signatures.back(), description.types)) {
      return std::unexpected(NativeInstantiationError{
          .status = NativeInstantiationStatus::ImportSignatureMismatch,
          .message = "external declaration does not match native signature for '" +
                     std::string(symbol) + "'",
      });
    }
    data_->imports.at(index) = std::move(binding);
  }
  return {};
}

std::expected<void, NativeInstantiationError> NativeInstance::InitializeConstants() {
  data_->global_states.resize(data_->image->global_initializers.size,
                              NativeProgramState::GlobalState::Uninitialized);
  auto& heap = data_->isolate->heap();
  auto* constants = heap.AllocateValueSlots(data_->image->strings.size);
  if (constants != nullptr) {
    heap.SetNativeProgramSlot(*anchor_.AsObject(), NativeProgramState::kConstants,
                              Value::Object(constants));
    auto* globals = heap.AllocateValueSlots(data_->image->global_initializers.size);
    if (globals != nullptr) {
      heap.SetNativeProgramSlot(*anchor_.AsObject(), NativeProgramState::kGlobals,
                                Value::Object(globals));
    }
  }
  if (data_->values.at(NativeProgramState::kGlobals).IsNull()) {
    return std::unexpected(NativeInstantiationError{
        .status = NativeInstantiationStatus::OutOfMemory,
        .message = "out of memory creating native program state",
    });
  }
  const auto strings = data_->image->strings.values();
  for (std::size_t index = 0; index < strings.size(); ++index) {
    auto* string = heap.AllocateString(strings.subspan(index).front().text());
    if (string == nullptr) {
      return std::unexpected(NativeInstantiationError{
          .status = NativeInstantiationStatus::OutOfMemory,
          .message = "out of memory creating native string constants",
      });
    }
    heap.SetValueSlot(*constants, index, Value::String(string));
  }
  return {};
}

std::expected<std::unique_ptr<NativeInstance>, NativeInstantiationError>
NativeInstance::Instantiate(Isolate& isolate, const NativeImage& image) {
  if (image.abi_version != kNativeAbiVersion || image.record_bytes != sizeof(NativeImage) ||
      image.abi_identity.text() != NativeRuntimeAbiIdentity()) {
    return std::unexpected(NativeInstantiationError{
        .status = NativeInstantiationStatus::AbiMismatch,
        .message = "native image and runtime ABI identities do not match",
    });
  }
  if (image.target.text() != NativeTargetIdentity()) {
    return std::unexpected(NativeInstantiationError{
        .status = NativeInstantiationStatus::TargetMismatch,
        .message = "native image and runtime targets do not match",
    });
  }
  std::unique_ptr<NativeProgramState> data(new (std::nothrow) NativeProgramState);
  if (data == nullptr) {
    return std::unexpected(NativeInstantiationError{
        .status = NativeInstantiationStatus::OutOfMemory,
        .message = "out of memory creating native program state",
    });
  }
  data->isolate = &isolate;
  data->image = &image;
  std::unique_ptr<NativeInstance> instance(new (std::nothrow) NativeInstance(*data, Value::Null()));
  if (instance == nullptr) {
    return std::unexpected(NativeInstantiationError{
        .status = NativeInstantiationStatus::OutOfMemory,
        .message = "out of memory creating native program instance",
    });
  }
  const auto description = DescribeLayouts(image);
  const auto bound = instance->BindImports(description);
  if (!bound) {
    return std::unexpected(bound.error());
  }
  LayoutTransaction transaction(isolate);
  const auto existing = isolate.native_images_.find(&image);
  auto cache = existing == isolate.native_images_.end() ? LayoutCache() : existing->second;
  if (cache.code_identity == nullptr) {
    cache.code_identity = isolate.CreateCodeIdentity();
  }
  instance->data_->layouts = InstantiateLayouts(description, isolate, cache);
  instance->data_->code_identity = cache.code_identity;
  std::vector<MemberDefinition> members;
  members.reserve(image.members.size);
  for (const auto& member : image.members.values()) {
    members.push_back({
        .receiver_type = member.receiver_type,
        .name = std::string(member.name.text()),
        .kind = static_cast<MemberKind>(member.kind),
        .target = member.target,
        .type = member.type == kNoNativeIndex ? std::nullopt : std::optional(member.type),
        .setter = member.setter == kNoNativeIndex ? std::nullopt : std::optional(member.setter),
    });
  }
  instance->data_->members = MemberTable(instance->data_->layouts.types, std::move(members));
  for (auto* type : instance->data_->layouts.classes) {
    if (type->callable()) {
      type->BindCodeIdentity(*cache.code_identity);
    }
  }
  auto* anchor = isolate.heap().AllocateNativeProgram(std::move(data));
  if (anchor == nullptr) {
    return std::unexpected(NativeInstantiationError{
        .status = NativeInstantiationStatus::OutOfMemory,
        .message = "out of memory publishing native program state",
    });
  }
  instance->anchor_ = Value::Object(anchor);
  const auto initialized = instance->InitializeConstants();
  if (!initialized) {
    return std::unexpected(initialized.error());
  }
  isolate.native_images_.insert_or_assign(&image, std::move(cache));
  transaction.Commit();
  return instance;
}

}  // namespace vela::runtime
