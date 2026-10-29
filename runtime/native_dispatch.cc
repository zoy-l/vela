#include "runtime/native_dispatch.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include "base/declaration.h"
#include "base/execution_status.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_abi.h"
#include "runtime/native_call.h"
#include "runtime/native_contracts.h"
#include "runtime/native_image.h"
#include "runtime/native_instance.h"
#include "runtime/native_program_state.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

struct ExportName final {
  std::string_view module;
  std::string_view name;
  bool operator==(const ExportName&) const = default;
  template <class H>
  friend H AbslHashValue(H state, const ExportName& key) {
    return H::combine(std::move(state), key.module, key.name);
  }
};

// 共享槽只拥有状态及 GC 值，不拥有初始化器或代码映像。初始化器始终由当前快照选择。
struct GlobalSlot final {
  GlobalSlot(Isolate& isolate, HeapObject& source)
      : storage(Value::Object(&source)), root(isolate.heap(), &storage, 1, RootOwner::Runtime) {}
  Value storage;
  RootRange root;
  NativeProgramState::GlobalState state = NativeProgramState::GlobalState::Uninitialized;
};

struct GlobalEntry final {
  std::shared_ptr<GlobalSlot> slot;
  std::uint32_t initializer = 0;
  std::uint32_t index = 0;
};

}  // namespace

struct NativeDispatchSnapshot final {
  NativeDispatchSnapshot(Isolate& isolate, Value source)
      : state(source),
        root(isolate.heap(), &state, 1, RootOwner::Runtime),
        preparation(isolate.external_resources(), ExternalResourceKind::NativeCandidate) {}
  Value state;
  RootRange root;
  ExternalResource preparation;
  absl::flat_hash_map<DeclarationId, std::uint32_t> functions;
  absl::flat_hash_map<ExportName, DeclarationId> exports;
  absl::flat_hash_map<DeclarationId, GlobalEntry> globals;
  NativeTypeIdentities type_identities;
  absl::flat_hash_map<std::pair<const NominalType*, DeclarationId>, std::uint32_t>
      interface_targets;
};

NativePreparedPublication::NativePreparedPublication(
    const NativeDispatch& owner,
    std::uint64_t base_revision,
    std::unique_ptr<NativeDispatchSnapshot> snapshot)
    : owner_(&owner), base_revision_(base_revision), snapshot_(std::move(snapshot)) {}
NativePreparedPublication::~NativePreparedPublication() = default;
NativePreparedPublication::NativePreparedPublication(NativePreparedPublication&&) noexcept =
    default;
NativePreparedPublication& NativePreparedPublication::operator=(
    NativePreparedPublication&&) noexcept = default;

NativeVersionEntry::Storage::Storage(Isolate& isolate, Value source, std::uint32_t target)
    : state(source), function(target), root(isolate.heap(), &state, 1, RootOwner::NativeHandle) {}
NativeVersionEntry::NativeVersionEntry(std::shared_ptr<const Storage> storage)
    : storage_(std::move(storage)) {}

FunctionIdentity NativeVersionEntry::identity() const noexcept {
  const auto& state = Heap::NativeProgram(*storage_->state.AsObject());
  return state.image->functions.values().subspan(storage_->function).front().identity;
}

std::expected<Value, ExecutionStatus> NativeVersionEntry::Invoke(
    std::span<const Value> arguments) const {
  auto instance = NativeInstance::Borrow(*storage_->state.AsObject());
  NativeExecution execution(instance);
  return InvokeNativeEntry(execution, storage_->function, arguments);
}

NativeNamedEntry::Storage::Storage(NativeDispatch& source, DeclarationId target)
    : dispatch(&source),
      declaration(target),
      resource(source.isolate().external_resources(), ExternalResourceKind::NativeEntry) {}
NativeNamedEntry::NativeNamedEntry(std::shared_ptr<const Storage> storage)
    : storage_(std::move(storage)) {}

DeclarationId NativeNamedEntry::declaration() const noexcept {
  return storage_->declaration;
}

std::expected<NativeVersionEntry, ExecutionStatus> NativeNamedEntry::Capture() const {
  const auto target = storage_->dispatch->Resolve(storage_->declaration);
  if (!target) {
    return std::unexpected(target.error());
  }
  return NativeVersionEntry(std::make_shared<const NativeVersionEntry::Storage>(
      storage_->dispatch->isolate(), Value::Object(target->state), target->function));
}

std::expected<Value, ExecutionStatus> NativeNamedEntry::Invoke(
    std::span<const Value> arguments) const {
  const auto target = storage_->dispatch->Resolve(storage_->declaration);
  if (!target) {
    return std::unexpected(target.error());
  }
  auto instance = NativeInstance::Borrow(*target->state);
  NativeExecution execution(instance);
  return InvokeNativeEntry(execution, target->function, arguments);
}

NativeDispatch::NativeDispatch(Isolate& isolate) noexcept : isolate_(&isolate) {}
NativeDispatch::~NativeDispatch() = default;

HeapObject& NativeDispatch::current_state() const noexcept {
  return *current_->state.AsObject();
}

std::uint64_t NativeDispatch::revision() const noexcept {
  return current_ == nullptr ? 0 : Heap::NativeProgram(*current_->state.AsObject()).image->revision;
}

std::expected<NativePreparedPublication, NativePublicationError> NativeDispatch::Prepare(
    std::uint64_t base_revision,
    std::shared_ptr<const NativeImage> image) {
  if (closed_ || isolate_->native_control().IsStopRequested()) {
    return std::unexpected(NativePublicationError{.status = NativePublicationStatus::Stopped,
                                                  .message = "native publication is stopped",
                                                  .declaration = {}});
  }
  if (base_revision != revision()) {
    return std::unexpected(NativePublicationError{.status = NativePublicationStatus::Stale,
                                                  .message = "native publication baseline is stale",
                                                  .declaration = {}});
  }
  if (image->native_mode != static_cast<std::uint32_t>(NativeProgramMode::Reloadable) ||
      image->revision != base_revision + 1) {
    return std::unexpected(NativePublicationError{
        .message = "reloadable native image must advance the publication revision by one",
        .declaration = {}});
  }
  auto snapshot = std::make_unique<NativeDispatchSnapshot>(*isolate_, Value::Null());
  const auto& description = *image;
  const auto* previous_image =
      current_ == nullptr ? nullptr : Heap::NativeProgram(*current_->state.AsObject()).image;
  if (previous_image != nullptr) {
    const auto compatible = CheckNativeNominalContracts(*previous_image, description);
    if (!compatible) {
      const auto& error = compatible.error();
      return std::unexpected(
          NativePublicationError{.status = NativePublicationStatus::RequiresReload,
                                 .message = error.message,
                                 .declaration = error.declaration});
    }
  }
  const auto functions = description.functions.values();
  for (std::size_t index = 0; index < functions.size(); ++index) {
    const auto& function = functions[index];
    const auto identity = function.identity;
    if (identity.revision != description.revision) {
      return std::unexpected(NativePublicationError{
          .message = "function version does not belong to the candidate image",
          .declaration = identity.declaration});
    }
    if (identity.local != 0) {
      continue;
    }
    if (identity.declaration == DeclarationId{} ||
        !snapshot->functions.emplace(identity.declaration, static_cast<std::uint32_t>(index))
             .second) {
      return std::unexpected(
          NativePublicationError{.message = "named function requires a unique declaration identity",
                                 .declaration = identity.declaration});
    }
    if (previous_image != nullptr) {
      const auto existing = current_->functions.find(identity.declaration);
      if (existing != current_->functions.end()) {
        const auto& previous = previous_image->functions.values()[existing->second];
        const auto generated = (function.flags & kNativeGeneratedConstructor) != 0;
        const auto was_generated = (previous.flags & kNativeGeneratedConstructor) != 0;
        if (generated != was_generated) {
          return std::unexpected(
              NativePublicationError{.status = NativePublicationStatus::RequiresReload,
                                     .message = "constructor kind changed; Reload is required",
                                     .declaration = identity.declaration});
        }
        const auto contract =
            generated && was_generated
                ? std::expected<void, std::string>{}
                : CheckNativeCallContract(*previous_image, previous, description, function);
        if (!contract) {
          return std::unexpected(
              NativePublicationError{.status = NativePublicationStatus::RequiresReload,
                                     .message = contract.error(),
                                     .declaration = identity.declaration});
        }
      }
    }
  }
  absl::flat_hash_map<DeclarationId, std::string_view> modules;
  for (const auto& declaration : description.declarations.values()) {
    if (declaration.kind == static_cast<std::uint32_t>(DeclarationKind::Module)) {
      modules.emplace(declaration.identity, declaration.name.text());
    }
  }
  for (const auto& declaration : description.declarations.values()) {
    const auto module = modules.find(declaration.owner);
    const auto target = snapshot->functions.find(declaration.identity);
    if (module == modules.end() || target == snapshot->functions.end()) {
      continue;
    }
    const auto& function = functions[target->second];
    if ((function.flags & kNativeExportedFunction) != 0 && function.hidden_parameters == 0) {
      snapshot->exports.emplace(ExportName{module->second, declaration.name.text()},
                                declaration.identity);
    }
  }
  const auto declarations = description.global_declarations.values();
  const auto initializers = description.global_initializers.values();
  const auto global_flags = description.global_flags.values();
  if (declarations.size() != initializers.size() || global_flags.size() != initializers.size()) {
    return std::unexpected(NativePublicationError{
        .message = "reloadable globals require stable declaration identities", .declaration = {}});
  }
  for (std::size_t index = 0; index < declarations.size(); ++index) {
    const auto identity = declarations[index];
    std::shared_ptr<GlobalSlot> slot;
    if (previous_image != nullptr) {
      const auto existing = current_->globals.find(identity);
      if (existing != current_->globals.end()) {
        const auto old_index = existing->second.initializer;
        const auto previous_flags = previous_image->global_flags.values()[existing->second.index];
        const auto& previous = previous_image->functions.values()[old_index];
        const auto& candidate = functions[initializers[index]];
        if (previous_flags != global_flags[index] ||
            !IsSameNativeType(*previous_image, previous_image->types.values()[previous.result_type],
                              description, description.types.values()[candidate.result_type])) {
          return std::unexpected(NativePublicationError{
              .status = NativePublicationStatus::RequiresReload,
              .message = "global slot type or mutability changed; Reload is required",
              .declaration = identity});
        }
        slot = existing->second.slot;
      }
    }
    if (slot == nullptr) {
      auto* storage = isolate_->heap().AllocateValueSlots(1);
      if (storage == nullptr) {
        return std::unexpected(
            NativePublicationError{.status = NativePublicationStatus::InstantiationFailed,
                                   .message = "out of memory preparing reloadable global slots",
                                   .declaration = identity});
      }
      slot = std::make_shared<GlobalSlot>(*isolate_, *storage);
    }
    if (identity == DeclarationId{} ||
        !snapshot->globals
             .emplace(identity, GlobalEntry{std::move(slot), initializers[index],
                                            static_cast<std::uint32_t>(index)})
             .second) {
      return std::unexpected(
          NativePublicationError{.message = "global slot requires a unique declaration identity",
                                 .declaration = identity});
    }
  }
  if (current_ != nullptr) {
    snapshot->type_identities = current_->type_identities;
  }
  const auto& source_image = *image;
  const auto created = NativeInstance::Instantiate(*isolate_, source_image, std::move(image),
                                                   &snapshot->type_identities);
  if (!created) {
    return std::unexpected(
        NativePublicationError{.status = NativePublicationStatus::InstantiationFailed,
                               .message = created.error().message,
                               .declaration = {}});
  }
  const auto state = (*created)->state().front();
  const auto classes = description.classes.values();
  const auto selectors = description.selector_declarations.values();
  const auto& layouts = (*created)->layouts().classes;
  for (std::size_t index = 0; index < classes.size(); ++index) {
    const auto targets = classes[index].dispatch_targets.values();
    const auto* nominal = &layouts[index]->nominal_type();
    for (std::size_t selector = 0; selector < targets.size(); ++selector) {
      const auto target = targets[selector];
      if (target != kInvalidFunctionTarget) {
        snapshot->interface_targets.emplace(std::pair(nominal, selectors[selector]), target);
      }
    }
  }
  snapshot->state = state;
  snapshot->root.Attach(isolate_->heap(), &snapshot->state, 1);
  Heap::NativeProgram(*state.AsObject()).dispatch = this;
  return NativePreparedPublication(*this, base_revision, std::move(snapshot));
}

std::expected<void, NativePublicationStatus> NativeDispatch::Publish(
    NativePreparedPublication&& candidate) noexcept {
  if (closed_ || isolate_->native_control().IsStopRequested()) {
    return std::unexpected(NativePublicationStatus::Stopped);
  }
  if (candidate.owner_ != this || candidate.base_revision_ != revision()) {
    return std::unexpected(NativePublicationStatus::Stale);
  }
  if (!isolate_->native_control().CanPublish()) {
    return std::unexpected(NativePublicationStatus::Busy);
  }
  candidate.snapshot_->preparation.Release();
  current_ = std::move(candidate.snapshot_);
  return {};
}

std::expected<NativeNamedEntry, ExecutionStatus> NativeDispatch::FindEntry(
    std::string_view module_uri,
    std::string_view name) {
  if (closed_) {
    return std::unexpected(ExecutionStatus::ExecutionStopped);
  }
  if (current_ == nullptr) {
    return std::unexpected(ExecutionStatus::InvalidEntry);
  }
  const auto found = current_->exports.find(ExportName{module_uri, name});
  if (found == current_->exports.end()) {
    return std::unexpected(ExecutionStatus::InvalidEntry);
  }
  return NativeNamedEntry(std::make_shared<const NativeNamedEntry::Storage>(*this, found->second));
}

std::expected<NativeResolvedDeclaration, ExecutionStatus> NativeDispatch::Resolve(
    DeclarationId declaration) const noexcept {
  if (closed_) {
    return std::unexpected(ExecutionStatus::ExecutionStopped);
  }
  if (current_ == nullptr) {
    return std::unexpected(ExecutionStatus::InvalidEntry);
  }
  const auto found = current_->functions.find(declaration);
  if (found == current_->functions.end()) {
    return std::unexpected(ExecutionStatus::TargetDeleted);
  }
  return NativeResolvedDeclaration{.state = current_->state.AsObject(), .function = found->second};
}

std::expected<NativeResolvedDeclaration, ExecutionStatus> NativeDispatch::ResolveInterface(
    const NominalType& receiver,
    DeclarationId selector) const noexcept {
  if (closed_) {
    return std::unexpected(ExecutionStatus::ExecutionStopped);
  }
  const auto found = current_->interface_targets.find(std::pair(&receiver, selector));
  if (found == current_->interface_targets.end()) {
    return std::unexpected(ExecutionStatus::TargetDeleted);
  }
  return NativeResolvedDeclaration{.state = current_->state.AsObject(), .function = found->second};
}

std::expected<Value, ExecutionStatus> NativeDispatch::LoadGlobal(NativeExecution& execution,
                                                                 DeclarationId declaration) {
  if (closed_) {
    return std::unexpected(ExecutionStatus::ExecutionStopped);
  }
  const auto found = current_->globals.find(declaration);
  if (found == current_->globals.end()) {
    return std::unexpected(ExecutionStatus::TargetDeleted);
  }
  const auto& entry = found->second;
  auto& slot = *entry.slot;
  using State = NativeProgramState::GlobalState;
  if (slot.state == State::Ready) {
    return Heap::ValueSlots(*slot.storage.AsObject()).front();
  }
  if (slot.state == State::Initializing) {
    return std::unexpected(ExecutionStatus::CyclicInitialization);
  }
  const NativeInitializationScope initialization(isolate_->native_control());
  auto instance = NativeInstance::Borrow(*current_->state.AsObject());
  const NativeInstanceScope scope(execution, instance);
  slot.state = State::Initializing;
  NativeValue result;
  const auto status =
      static_cast<ExecutionStatus>(instance.image().functions.values()[entry.initializer].entry(
          &execution, nullptr, 0, &result));
  if (status != ExecutionStatus::Returned) {
    slot.state = State::Uninitialized;
    return std::unexpected(status);
  }
  const auto value = DecodeNativeValue(result);
  isolate_->heap().SetValueSlot(*slot.storage.AsObject(), 0, value);
  slot.state = State::Ready;
  return value;
}

std::expected<void, ExecutionStatus> NativeDispatch::StoreGlobal(DeclarationId declaration,
                                                                 Value value) noexcept {
  if (closed_) {
    return std::unexpected(ExecutionStatus::ExecutionStopped);
  }
  const auto found = current_->globals.find(declaration);
  if (found == current_->globals.end()) {
    return std::unexpected(ExecutionStatus::TargetDeleted);
  }
  auto& slot = *found->second.slot;
  isolate_->heap().SetValueSlot(*slot.storage.AsObject(), 0, value);
  slot.state = NativeProgramState::GlobalState::Ready;
  return {};
}

extern "C" std::uint8_t vela_native_v3_load_global(NativeExecution* execution,
                                                   std::uint64_t declaration,
                                                   NativeValue* result) {
  auto& dispatch = *Heap::NativeProgram(*execution->instance().state().front().AsObject()).dispatch;
  const auto loaded = dispatch.LoadGlobal(*execution, DeclarationId{declaration});
  if (!loaded) {
    return static_cast<std::uint8_t>(loaded.error());
  }
  *result = EncodeNativeValue(*loaded);
  return static_cast<std::uint8_t>(ExecutionStatus::Returned);
}

extern "C" std::uint8_t vela_native_v3_store_global(NativeExecution* execution,
                                                    std::uint64_t declaration,
                                                    const NativeValue* value) noexcept {
  auto& dispatch = *Heap::NativeProgram(*execution->instance().state().front().AsObject()).dispatch;
  const auto stored = dispatch.StoreGlobal(DeclarationId{declaration}, DecodeNativeValue(*value));
  return static_cast<std::uint8_t>(stored ? ExecutionStatus::Returned : stored.error());
}

void NativeDispatch::Close() noexcept {
  closed_ = true;
  current_.reset();
}

}  // namespace vela::runtime
