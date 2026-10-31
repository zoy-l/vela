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

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/callable.h"
#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/layout_description.h"
#include "base/member.h"
#include "base/native_program_mode.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/layout_transaction.h"
#include "runtime/native_abi.h"
#include "runtime/native_build_identity.h"
#include "runtime/native_function.h"
#include "runtime/native_global_internal.h"
#include "runtime/native_image.h"
#include "runtime/native_program_state.h"
#include "runtime/object.h"
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

std::expected<void, NativeInstantiationError> ValidateImage(
    const NativeImage& image,
    const NativeTypeIdentities* identities) {
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
  if (image.native_mode == static_cast<std::uint32_t>(NativeProgramMode::Reloadable) &&
      identities == nullptr) {
    return std::unexpected(NativeInstantiationError{
        .status = NativeInstantiationStatus::MissingSession,
        .message = "reloadable images must be instantiated by their session dispatch",
    });
  }
  return {};
}

void PrepareFieldCaches(NativeProgramState& state) {
  const auto& image = *state.image;
  state.field_caches.reserve(image.functions.size);
  for (const auto& function : image.functions.values()) {
    state.field_caches.emplace_back(
        function.dependencies == nullptr ? 0 : function.dependencies->fields.size);
  }
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

void BindReloadableFields(layout::Class& description,
                          const NativeClassDescription& type,
                          std::vector<DeclarationId>& fields,
                          ClassLayoutOptions& options) {
  options.storage = ClassStorage::Reloadable;
  const auto declared = type.fields.values();
  absl::flat_hash_set<DeclarationId> present;
  for (const auto& field : declared) {
    present.insert(field.declaration);
  }
  absl::flat_hash_map<DeclarationId, std::uint32_t> slots;
  for (std::size_t slot = 0; slot < fields.size(); ++slot) {
    auto& field = fields.at(slot);
    if (!present.contains(field)) {
      field = {};
    } else {
      slots.emplace(field, static_cast<std::uint32_t>(slot));
    }
  }
  for (const auto& field : declared) {
    if (!slots.contains(field.declaration)) {
      slots.emplace(field.declaration, static_cast<std::uint32_t>(fields.size()));
      fields.push_back(field.declaration);
    }
  }
  for (auto& reference : description.reference_fields) {
    reference = slots.at(declared.subspan(reference).front().declaration);
  }
  description.field_count = static_cast<std::uint32_t>(fields.size());
  options.field_declarations = fields;
}

void RegisterInterfaces(const NativeImage& image,
                        const layout::Program& description,
                        Isolate& isolate,
                        NativeTypeIdentities& identities,
                        LayoutCache& cache) {
  const auto interface_ids = image.interface_declarations.values();
  for (std::size_t index = 0; index < description.interfaces.size(); ++index) {
    const auto identity = interface_ids.subspan(index).front();
    if (identity == DeclarationId{}) {
      continue;
    }
    const auto [entry, inserted] = identities.interfaces.try_emplace(identity, nullptr);
    if (inserted) {
      entry->second = isolate.CreateInterfaceType();
    }
    cache.interfaces.emplace(description.interfaces.at(index), entry->second);
  }
}

std::vector<ClassLayoutOptions> ReloadableClassOptions(const NativeImage& image,
                                                       layout::Program& description,
                                                       Isolate& isolate,
                                                       NativeTypeIdentities& identities,
                                                       LayoutCache& cache) {
  RegisterInterfaces(image, description, isolate, identities, cache);
  const auto classes = image.classes.values();
  std::vector<ClassLayoutOptions> result(classes.size());
  for (std::size_t index = 0; index < classes.size(); ++index) {
    const auto& type = classes.subspan(index).front();
    const auto kind = static_cast<layout::Class::Kind>(type.kind);
    if (type.declaration == DeclarationId{} || kind == layout::Class::Kind::RuntimeError ||
        kind == layout::Class::Kind::DateParts) {
      continue;
    }
    const auto [entry, inserted] = identities.classes.try_emplace(type.declaration, nullptr);
    if (inserted) {
      std::vector<const InterfaceType*> interfaces;
      for (const auto interface : type.interfaces.values()) {
        interfaces.push_back(cache.interfaces.at(description.interfaces.at(interface)));
      }
      entry->second = isolate.CreateNominalType({
          .kind = kind == layout::Class::Kind::Enum ? NominalKind::Enum : NominalKind::Class,
          .interfaces = std::move(interfaces),
          .declaration = type.declaration,
      });
    }
    auto& options = result.at(index);
    options.nominal = entry->second;
    options.revision = image.revision;
    if (kind == layout::Class::Kind::Instance && type.callable_function == kNoNativeIndex) {
      auto& fields = identities.fields.try_emplace(type.declaration).first->second;
      BindReloadableFields(description.classes.at(index), type, fields, options);
    }
  }
  return result;
}

std::vector<MemberDefinition> DescribeMembers(const NativeImage& image,
                                              const ProgramLayouts& layouts) {
  std::vector<MemberDefinition> members;
  members.reserve(image.members.size);
  for (const auto& member : image.members.values()) {
    auto target = member.target;
    if (member.kind == static_cast<std::uint32_t>(MemberKind::Field)) {
      const auto& receiver = image.types.values().subspan(member.receiver_type).front();
      const auto* layout = layouts.classes.at(receiver.index);
      if (layout->storage() == ClassStorage::Reloadable) {
        const auto fields = image.classes.values().subspan(receiver.index).front().fields.values();
        target = *layout->FieldIndex(fields.subspan(target).front().declaration);
      }
    }
    members.push_back({
        .receiver_type = member.receiver_type,
        .name = std::string(member.name.text()),
        .kind = static_cast<MemberKind>(member.kind),
        .target = target,
        .type = member.type == kNoNativeIndex ? std::nullopt : std::optional(member.type),
        .setter = member.setter == kNoNativeIndex ? std::nullopt : std::optional(member.setter),
    });
  }
  return members;
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

NativeInstance::NativeInstance(NativeProgramState& data, Value anchor, RootOwner root_owner)
    : data_(&data), anchor_(anchor), anchor_root_(data.isolate->heap(), &anchor_, 1, root_owner) {}

NativeInstance NativeInstance::Borrow(HeapObject& state) {
  return {Heap::NativeProgram(state), Value::Object(&state), RootOwner::Runtime};
}

Value NativeInstance::constant(std::size_t index) const noexcept {
  return Heap::ValueSlots(*data_->values.at(NativeProgramState::kConstants).AsObject())
      .subspan(index)
      .front();
}

std::expected<Value, ExecutionStatus> NativeInstance::InitializeGlobal(NativeExecution& execution,
                                                                       std::size_t index) {
  auto& state = std::span(data_->global_states, data_->image->global_initializers.size)
                    .subspan(index)
                    .front();
  const auto function = data_->image->global_initializers.values().subspan(index).front();
  const auto entry = data_->image->functions.values().subspan(function).front().entry;
  const auto value = internal::InitializeNativeGlobal(execution, entry, state);
  if (!value) {
    return value;
  }
  StoreGlobal(index, *value);
  return value;
}

void NativeInstance::StoreGlobal(std::size_t index, Value value) noexcept {
  auto& state = std::span(data_->global_states, data_->image->global_initializers.size)
                    .subspan(index)
                    .front();
  internal::StoreNativeGlobal(data_->isolate->heap(),
                              *data_->values.at(NativeProgramState::kGlobals).AsObject(), index,
                              state, value);
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
  const auto& image = *data_->image;
  auto& heap = data_->isolate->heap();
  auto* constants = heap.AllocateValueSlots(image.strings.size);
  if (constants == nullptr) {
    return std::unexpected(NativeInstantiationError{
        .status = NativeInstantiationStatus::OutOfMemory,
        .message = "out of memory creating native string constant slots",
    });
  }
  heap.SetNativeProgramSlot(*anchor_.AsObject(), NativeProgramState::kConstants,
                            Value::Object(constants));
  if (image.native_mode == static_cast<std::uint32_t>(NativeProgramMode::Closed)) {
    const auto global_count = image.global_initializers.size;
    std::unique_ptr<NativeProgramState::GlobalState[]> global_states(
        new (std::nothrow) NativeProgramState::GlobalState[global_count]{});
    data_->global_states = global_states.release();
    if (data_->global_states == nullptr) {
      return std::unexpected(NativeInstantiationError{
          .status = NativeInstantiationStatus::OutOfMemory,
          .message = "out of memory creating native global states",
      });
    }
    auto* globals = heap.AllocateValueSlots(global_count);
    if (globals != nullptr) {
      heap.SetNativeProgramSlot(*anchor_.AsObject(), NativeProgramState::kGlobals,
                                Value::Object(globals));
    }
    if (data_->values.at(NativeProgramState::kGlobals).IsNull()) {
      return std::unexpected(NativeInstantiationError{
          .status = NativeInstantiationStatus::OutOfMemory,
          .message = "out of memory creating native program state",
      });
    }
  }
  const auto strings = image.strings.values();
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
  return Instantiate(isolate, image, {}, nullptr);
}

std::expected<std::unique_ptr<NativeInstance>, NativeInstantiationError>
NativeInstance::Instantiate(Isolate& isolate, std::shared_ptr<const NativeImage> image) {
  const auto& description = *image;
  return Instantiate(isolate, description, std::move(image), nullptr);
}

std::expected<std::unique_ptr<NativeInstance>, NativeInstantiationError>
NativeInstance::Instantiate(Isolate& isolate,
                            const NativeImage& image,
                            std::shared_ptr<const NativeImage> owner,
                            NativeTypeIdentities* identities) {
  if (const auto validated = ValidateImage(image, identities); !validated) {
    return std::unexpected(validated.error());
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
  data->image_owner = std::move(owner);
  if (identities != nullptr) {
    PrepareFieldCaches(*data);
  }
  std::unique_ptr<NativeInstance> instance(
      new (std::nothrow) NativeInstance(*data, Value::Null(), RootOwner::NativeInstance));
  if (instance == nullptr) {
    return std::unexpected(NativeInstantiationError{
        .status = NativeInstantiationStatus::OutOfMemory,
        .message = "out of memory creating native program instance",
    });
  }
  auto& state = *instance->data_;
  auto description = DescribeLayouts(image);
  const auto bound = instance->BindImports(description);
  if (!bound) {
    return std::unexpected(bound.error());
  }
  const auto closed = image.native_mode == static_cast<std::uint32_t>(NativeProgramMode::Closed);
  LayoutTransaction transaction(isolate,
                                closed ? LayoutLifetime::Isolate : LayoutLifetime::Reloadable);
  const auto existing = isolate.native_images_.find(&image);
  const auto has_owner = state.image_owner != nullptr;
  const auto can_reuse = closed && existing != isolate.native_images_.end() &&
                         existing->second.static_lifetime == !has_owner &&
                         (!has_owner || !existing->second.owner.expired());
  auto cache = can_reuse ? existing->second.layouts : LayoutCache();
  if (cache.code_identity == nullptr) {
    cache.code_identity = isolate.CreateCodeIdentity();
  }
  std::vector<ClassLayoutOptions> class_options;
  if (identities != nullptr) {
    class_options = ReloadableClassOptions(image, description, isolate, *identities, cache);
  }
  state.layouts = InstantiateLayouts(description, isolate, cache, class_options);
  state.code_identity = cache.code_identity;
  auto members = DescribeMembers(image, state.layouts);
  state.members = MemberTable(state.layouts.types, std::move(members));
  for (auto* type : state.layouts.classes) {
    if (type->callable() != nullptr) {
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
    // 常量对象尚未逃逸；先清除候选根并回收其对象，再回滚被它们借用的布局。
    instance.reset();
    isolate.heap().FinishCollection();
    isolate.heap().Collect();
    return std::unexpected(initialized.error());
  }
  if (closed && has_owner) {
    absl::erase_if(isolate.native_images_, [](const auto& entry) {
      const auto& cached = entry.second;
      return !cached.static_lifetime && cached.owner.expired();
    });
  }
  if (closed) {
    isolate.native_images_.insert_or_assign(&image, Isolate::NativeImageCache{
                                                        .layouts = std::move(cache),
                                                        .owner = state.image_owner,
                                                        .static_lifetime = !has_owner,
                                                    });
  }
  if (closed) {
    transaction.Commit();
  } else {
    transaction.CommitNative(*anchor);
  }
  return instance;
}

}  // namespace vela::runtime
