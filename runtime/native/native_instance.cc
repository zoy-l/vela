#include "runtime/native/native_instance.h"
#include "runtime/isolate/external_resources.h"

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
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/isolate/layout_transaction.h"
#include "runtime/isolate/program_layouts.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_global_internal.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_program_state.h"
#include "runtime/native_build_identity.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

std::unexpected<NativeInstantiationError> OutOfMemory(std::string message) {
  return std::unexpected(NativeInstantiationError{
      .status = NativeInstantiationStatus::OutOfMemory,
      .message = std::move(message),
  });
}

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
      .native_key = Text(source.native_key),
      .native_contract = Text(source.native_contract),
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
  if (image.IsReloadable() && identities == nullptr) {
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
    const auto fields = function.dependencies == nullptr ? std::span<const DeclarationId>{}
                                                         : function.dependencies->fields.values();
    // 源位置依赖保留每次访问；物理槽缓存按稳定字段身份合并读写和重复访问。
    const absl::flat_hash_set<DeclarationId> unique(fields.begin(), fields.end());
    state.field_caches.emplace_back(unique.size());
  }
}

void PrepareDirectFieldLayouts(NativeProgramState& state) {
  const auto classes = state.image->classes.values();
  state.direct_field_layouts.reserve(classes.size());
  for (std::size_t index = 0; index < classes.size(); ++index) {
    const auto& description = classes[index];
    const auto* layout = state.layouts.classes[index];
    const auto canonical =
        std::ranges::equal(description.fields.values(), layout->field_declarations(), {},
                           &NativeFieldDescription::declaration);
    state.direct_field_layouts.push_back(canonical ? layout : nullptr);
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
      .nominals = {},
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
    auto& field = fields[slot];
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
    reference = slots.at(declared[reference].declaration);
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
    const auto identity = interface_ids[index];
    if (identity == DeclarationId{}) {
      continue;
    }
    const auto [entry, inserted] = identities.interfaces.try_emplace(identity, nullptr);
    if (inserted) {
      entry->second = isolate.CreateInterfaceType(identity);
    }
    cache.interfaces.emplace(description.interfaces[index], entry->second);
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
    const auto& type = classes[index];
    const auto kind = static_cast<layout::Class::Kind>(type.kind);
    if (kind == layout::Class::Kind::Host) {
      auto* layout = isolate.FindNativeClass(type.native_key.text())->layout;
      identities.classes.try_emplace(type.declaration, &layout->nominal_type());
      continue;
    }
    if (type.declaration == DeclarationId{} || kind == layout::Class::Kind::RuntimeError ||
        kind == layout::Class::Kind::DateParts) {
      continue;
    }
    const auto [entry, inserted] = identities.classes.try_emplace(type.declaration, nullptr);
    if (inserted) {
      std::vector<const InterfaceType*> interfaces;
      for (const auto interface : type.interfaces.values()) {
        interfaces.push_back(cache.interfaces.at(description.interfaces[interface]));
      }
      entry->second = isolate.CreateNominalType({
          .kind = kind == layout::Class::Kind::Enum ? NominalKind::Enum : NominalKind::Class,
          .interfaces = std::move(interfaces),
          .declaration = type.declaration,
      });
    }
    auto& options = result[index];
    options.nominal = entry->second;
    if (kind == layout::Class::Kind::Instance && type.callable_function == kNoNativeIndex) {
      auto& fields = identities.fields.try_emplace(type.declaration).first->second;
      BindReloadableFields(description.classes[index], type, fields, options);
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
      const auto& receiver = image.types.values()[member.receiver_type];
      const auto* layout = layouts.classes[receiver.index];
      if (layout->storage() == ClassStorage::Reloadable) {
        const auto fields = image.classes.values()[receiver.index].fields.values();
        const auto field = layout->FieldIndex(fields[target].declaration);
        if (!field) {
          std::unreachable();
        }
        target = *field;
      }
    }
    members.push_back({
        .receiver_type = member.receiver_type,
        .name = std::string(member.name.text()),
        .kind = static_cast<MemberKind>(member.kind),
        .target = target,
        .type = OptionalIndex(member.type),
        .setter = OptionalIndex(member.setter),
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
      .receiver = OptionalIndex(source.receiver_type),
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

NativeInstance::NativeInstance(NativeProgramState& data,
                               Value anchor,
                               std::optional<ExternalResourceKind> root_owner)
    : data_(&data), anchor_(anchor), anchor_root_(data.isolate->heap(), &anchor_, 1, root_owner) {}

NativeInstance NativeInstance::Borrow(HeapObject& state) {
  return {Heap::NativeProgram(state), Value::Object(&state), std::nullopt};
}

std::expected<Value, ExecutionStatus> NativeInstance::InitializeGlobal(NativeExecution& execution,
                                                                       std::size_t index) {
  auto& state = data_->global_state(index);
  const auto function = data_->image->global_initializers.values()[index];
  const auto entry = data_->image->function(function).entry;
  auto& storage = *data_->values[NativeProgramState::kGlobals].AsObject();
  return internal::InitializeNativeGlobal(execution, entry, storage, index, state);
}

std::expected<void, NativeInstantiationError> NativeInstance::BindImports(
    const layout::Program& description) {
  for (const auto& type : description.classes) {
    if (type.kind != layout::Class::Kind::Host) {
      continue;
    }
    const auto* binding = data_->isolate->FindNativeClass(type.native_key);
    if (binding == nullptr) {
      return std::unexpected(NativeInstantiationError{
          .status = NativeInstantiationStatus::MissingImport,
          .message = "unregistered native class '" + type.native_key + "'",
      });
    }
    if (binding->description->contract != type.native_contract) {
      return std::unexpected(NativeInstantiationError{
          .status = NativeInstantiationStatus::ImportSignatureMismatch,
          .message = "external class contract does not match registered native class '" +
                     type.native_key + "'",
      });
    }
  }
  const auto functions = data_->image->functions.values();
  data_->signatures.reserve(functions.size());
  data_->imports.resize(functions.size());
  for (std::size_t index = 0; index < functions.size(); ++index) {
    const auto& function = functions[index];
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
    if (!binding->IsSignatureCompatible(data_->signatures.back(), description)) {
      return std::unexpected(NativeInstantiationError{
          .status = NativeInstantiationStatus::ImportSignatureMismatch,
          .message = "external declaration does not match native signature for '" +
                     std::string(symbol) + "'",
      });
    }
    data_->imports[index] = std::move(binding);
  }
  return {};
}

std::expected<void, NativeInstantiationError> NativeInstance::InitializeConstants() {
  const auto& image = *data_->image;
  auto& heap = data_->isolate->heap();
  auto* constants = heap.AllocateValueSlots(image.strings.size);
  if (constants == nullptr) {
    return OutOfMemory("out of memory creating native string constant slots");
  }
  heap.SetNativeProgramSlot(*anchor_.AsObject(), NativeProgramState::kConstants,
                            Value::Object(constants));
  if (!image.IsReloadable()) {
    const auto global_count = image.global_initializers.size;
    std::unique_ptr<NativeProgramState::GlobalState[]> global_states(
        new (std::nothrow) NativeProgramState::GlobalState[global_count]{});
    data_->global_states = global_states.release();
    if (data_->global_states == nullptr) {
      return OutOfMemory("out of memory creating native global states");
    }
    auto* globals = heap.AllocateValueSlots(global_count);
    if (globals == nullptr) {
      return OutOfMemory("out of memory creating native program state");
    }
    heap.SetNativeProgramSlot(*anchor_.AsObject(), NativeProgramState::kGlobals,
                              Value::Object(globals));
  } else {
    std::unique_ptr<NativeGlobalCache[]> caches(
        new (std::nothrow) NativeGlobalCache[image.global_initializers.size]{});
    data_->global_caches = caches.release();
    if (data_->global_caches == nullptr) {
      return OutOfMemory("out of memory creating reloadable global bindings");
    }
  }
  const auto strings = image.strings.values();
  for (std::size_t index = 0; index < strings.size(); ++index) {
    auto* string = heap.AllocateString(strings[index].text());
    if (string == nullptr) {
      return OutOfMemory("out of memory creating native string constants");
    }
    heap.SetValueSlot(*constants, index, Value::String(string));
  }
  return {};
}

std::optional<std::uint32_t> NativeInstance::FindFunction(std::string_view name) const noexcept {
  const auto functions = image().functions.values();
  for (std::size_t index = 0; index < functions.size(); ++index) {
    if (functions[index].name.text() == name) {
      return static_cast<std::uint32_t>(index);
    }
  }
  return std::nullopt;
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
    return OutOfMemory("out of memory creating native program state");
  }
  data->isolate = &isolate;
  data->image = &image;
  data->image_owner = std::move(owner);
  const auto closed = !image.IsReloadable();
  if (!closed) {
    PrepareFieldCaches(*data);
  }
  std::unique_ptr<NativeInstance> instance(new (std::nothrow) NativeInstance(
      *data, Value::Null(), ExternalResourceKind::NativeInstance));
  if (instance == nullptr) {
    return OutOfMemory("out of memory creating native program instance");
  }
  auto& state = *instance->data_;
  auto description = DescribeLayouts(image);
  const auto bound = instance->BindImports(description);
  if (!bound) {
    return std::unexpected(bound.error());
  }
  LayoutTransaction transaction(isolate, LayoutLifetime::Collectible);
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
  if (!closed) {
    class_options = ReloadableClassOptions(image, description, isolate, *identities, cache);
  }
  state.layouts = InstantiateLayouts(description, isolate, cache, class_options);
  if (!closed) {
    PrepareDirectFieldLayouts(state);
  }
  state.code_identity = cache.code_identity;
  auto members = DescribeMembers(image, state.layouts);
  state.members = MemberTable(state.layouts.types, std::move(members));
  for (std::size_t index = 0; index < state.layouts.classes.size(); ++index) {
    const auto* nominal = &state.layouts.classes[index]->nominal_type();
    state.class_indices.emplace(nominal, static_cast<std::uint32_t>(index));
    const auto names = image.classes.values()[index].enum_members.values();
    for (std::size_t ordinal = 0; ordinal < names.size(); ++ordinal) {
      state.enum_members.emplace(std::pair(nominal, names[ordinal].text()),
                                 static_cast<std::uint32_t>(ordinal));
    }
  }
  for (auto* type : state.layouts.classes) {
    if (type->callable() != nullptr) {
      type->BindCodeIdentity(*cache.code_identity);
    }
  }
  auto* anchor = isolate.heap().AllocateNativeProgram(std::move(data));
  if (anchor == nullptr) {
    return OutOfMemory("out of memory publishing native program state");
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
  if (closed) {
    isolate.native_images_.insert_or_assign(&image, Isolate::NativeImageCache{
                                                        .layouts = std::move(cache),
                                                        .owner = state.image_owner,
                                                        .static_lifetime = !has_owner,
                                                    });
    transaction.CommitImage();
  } else {
    transaction.CommitNative(*anchor);
  }
  return instance;
}

}  // namespace vela::runtime
