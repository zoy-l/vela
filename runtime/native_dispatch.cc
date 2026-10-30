#include "runtime/native_dispatch.h"
#include "runtime/native_deoptimization.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/callable.h"
#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "runtime/external_resources.h"
#include "runtime/heap.h"
#include "runtime/instance_storage.h"
#include "runtime/isolate.h"
#include "runtime/native_abi.h"
#include "runtime/native_call.h"
#include "runtime/native_contracts.h"
#include "runtime/native_control.h"
#include "runtime/native_image.h"
#include "runtime/native_instance.h"
#include "runtime/native_materialization.h"
#include "runtime/native_metadata.h"
#include "runtime/native_program_state.h"
#include "runtime/native_version.h"
#include "runtime/object.h"
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
  std::expected<void, NativePublicationError> IndexFunctions(
      const NativeImage& description,
      const NativeDispatchSnapshot* previous_snapshot);
  void IndexExports(const NativeImage& description);
  std::expected<void, NativePublicationError> PrepareGlobals(
      Isolate& isolate,
      const NativeImage& description,
      const NativeDispatchSnapshot* previous_snapshot);
  void IndexLayouts(const NativeImage& description, const NativeInstance& instance);
  Value state;
  RootRange root;
  ExternalResource preparation;
  absl::flat_hash_map<DeclarationId, std::uint32_t> functions;
  absl::flat_hash_map<ExportName, DeclarationId> exports;
  absl::flat_hash_map<DeclarationId, GlobalEntry> globals;
  NativeTypeIdentities type_identities;
  absl::flat_hash_map<const NominalType*, const ClassLayout*> classes;
  absl::flat_hash_map<DeclarationId, std::uint32_t> field_initializers;
  absl::flat_hash_map<std::pair<const NominalType*, DeclarationId>, std::uint32_t>
      interface_targets;
};

std::expected<void, NativePublicationError> NativeDispatchSnapshot::IndexFunctions(
    const NativeImage& description,
    const NativeDispatchSnapshot* previous_snapshot) {
  const auto* previous_image =
      previous_snapshot == nullptr
          ? nullptr
          : Heap::NativeProgram(*previous_snapshot->state.AsObject()).image;
  const auto function_descriptions = description.functions.values();
  for (std::size_t index = 0; index < function_descriptions.size(); ++index) {
    const auto& function = function_descriptions.subspan(index).front();
    const auto identity = function.identity;
    if (identity.revision != description.revision) {
      return std::unexpected(NativePublicationError{
          .message = "function version does not belong to the candidate image",
          .declaration = identity.declaration,
      });
    }
    if (identity.local != 0) {
      continue;
    }
    if (identity.declaration == DeclarationId{} ||
        !functions.emplace(identity.declaration, static_cast<std::uint32_t>(index)).second) {
      return std::unexpected(NativePublicationError{
          .message = "named function requires a unique declaration identity",
          .declaration = identity.declaration,
      });
    }
    if (previous_image != nullptr) {
      const auto existing = previous_snapshot->functions.find(identity.declaration);
      if (existing != previous_snapshot->functions.end()) {
        const auto& previous = previous_image->functions.values().subspan(existing->second).front();
        const auto contract =
            CheckNativeCallContract(*previous_image, previous, description, function);
        if (!contract) {
          return std::unexpected(NativePublicationError{
              .status = NativePublicationStatus::RequiresReload,
              .message = contract.error(),
              .declaration = identity.declaration,
          });
        }
      }
    }
  }
  return {};
}

void NativeDispatchSnapshot::IndexExports(const NativeImage& description) {
  absl::flat_hash_map<DeclarationId, std::string_view> modules;
  for (const auto& declaration : description.declarations.values()) {
    if (declaration.kind == static_cast<std::uint32_t>(DeclarationKind::Module)) {
      modules.emplace(declaration.identity, declaration.name.text());
    }
  }
  for (const auto& declaration : description.declarations.values()) {
    const auto module = modules.find(declaration.owner);
    const auto target = functions.find(declaration.identity);
    if (module == modules.end() || target == functions.end()) {
      continue;
    }
    const auto& function = description.functions.values().subspan(target->second).front();
    if ((function.flags & kNativeExportedFunction) != 0 && function.hidden_parameters == 0) {
      exports.emplace(ExportName{.module = module->second, .name = declaration.name.text()},
                      declaration.identity);
    }
  }
}

std::expected<void, NativePublicationError> NativeDispatchSnapshot::PrepareGlobals(
    Isolate& isolate,
    const NativeImage& description,
    const NativeDispatchSnapshot* previous_snapshot) {
  const auto* previous_image =
      previous_snapshot == nullptr
          ? nullptr
          : Heap::NativeProgram(*previous_snapshot->state.AsObject()).image;
  const auto declarations = description.global_declarations.values();
  const auto initializers = description.global_initializers.values();
  const auto global_flags = description.global_flags.values();
  if (declarations.size() != initializers.size() || global_flags.size() != initializers.size()) {
    return std::unexpected(NativePublicationError{
        .message = "reloadable globals require stable declaration identities",
        .declaration = {},
    });
  }
  for (std::size_t index = 0; index < declarations.size(); ++index) {
    const auto identity = declarations.subspan(index).front();
    std::shared_ptr<GlobalSlot> slot;
    if (previous_image != nullptr) {
      const auto existing = previous_snapshot->globals.find(identity);
      if (existing != previous_snapshot->globals.end()) {
        const auto old_index = existing->second.initializer;
        const auto previous_flags =
            previous_image->global_flags.values().subspan(existing->second.index).front();
        const auto& previous = previous_image->functions.values().subspan(old_index).front();
        const auto& candidate =
            description.functions.values().subspan(initializers.subspan(index).front()).front();
        constexpr auto kBindingFlags = static_cast<std::uint32_t>(GlobalFlag::Constant);
        if ((previous_flags & kBindingFlags) !=
                (global_flags.subspan(index).front() & kBindingFlags) ||
            !IsSameNativeType(*previous_image,
                              previous_image->types.values().subspan(previous.result_type).front(),
                              description,
                              description.types.values().subspan(candidate.result_type).front())) {
          return std::unexpected(NativePublicationError{
              .status = NativePublicationStatus::RequiresReload,
              .message = "global slot type or mutability changed; Reload is required",
              .declaration = identity,
          });
        }
        slot = existing->second.slot;
      }
    }
    if (slot == nullptr) {
      auto* storage = isolate.heap().AllocateValueSlots(1);
      if (storage == nullptr) {
        return std::unexpected(NativePublicationError{
            .status = NativePublicationStatus::InstantiationFailed,
            .message = "out of memory preparing reloadable global slots",
            .declaration = identity,
        });
      }
      slot = std::make_shared<GlobalSlot>(isolate, *storage);
    }
    if (identity == DeclarationId{} ||
        !globals
             .emplace(identity,
                      GlobalEntry{
                          .slot = std::move(slot),
                          .initializer = initializers.subspan(index).front(),
                          .index = static_cast<std::uint32_t>(index),
                      })
             .second) {
      return std::unexpected(NativePublicationError{
          .message = "global slot requires a unique declaration identity",
          .declaration = identity,
      });
    }
  }
  return {};
}

void NativeDispatchSnapshot::IndexLayouts(const NativeImage& description,
                                          const NativeInstance& instance) {
  const auto class_descriptions = description.classes.values();
  const auto selectors = description.selector_declarations.values();
  const auto& layouts = instance.layouts().classes;
  for (std::size_t index = 0; index < class_descriptions.size(); ++index) {
    const auto& class_description = class_descriptions.subspan(index).front();
    const auto targets = class_description.dispatch_targets.values();
    const auto* layout = layouts.at(index);
    const auto* nominal = &layout->nominal_type();
    if (layout->storage() == ClassStorage::Reloadable) {
      classes.emplace(nominal, layout);
      for (const auto& field : class_description.fields.values()) {
        field_initializers.emplace(field.declaration, field.initializer);
      }
    }
    for (std::size_t selector = 0; selector < targets.size(); ++selector) {
      const auto target = targets.subspan(selector).front();
      if (target != kInvalidFunctionTarget) {
        interface_targets.emplace(std::pair(nominal, selectors.subspan(selector).front()), target);
      }
    }
  }
}

NativePreparedPublication::NativePreparedPublication(
    const NativeDispatch& owner,
    std::uint64_t base_revision,
    std::unique_ptr<NativeDispatchSnapshot> snapshot,
    std::chrono::nanoseconds preparation)
    : owner_(&owner),
      base_revision_(base_revision),
      snapshot_(std::move(snapshot)),
      preparation_(preparation) {}
NativePreparedPublication::~NativePreparedPublication() = default;
NativePreparedPublication::NativePreparedPublication(NativePreparedPublication&&) noexcept =
    default;
NativePreparedPublication& NativePreparedPublication::operator=(
    NativePreparedPublication&&) noexcept = default;

NativeVersionEntry::Storage::Storage(Value source, std::uint32_t target)
    : version(*source.AsObject(), target, RootOwner::NativeHandle) {}
NativeVersionEntry::NativeVersionEntry(std::shared_ptr<const Storage> storage)
    : storage_(std::move(storage)) {}

FunctionIdentity NativeVersionEntry::identity() const noexcept {
  const auto& version = storage_->version;
  const auto& state = Heap::NativeProgram(*version.state().AsObject());
  return state.image->functions.values().subspan(version.function()).front().identity;
}

std::expected<Value, ExecutionStatus> NativeVersionEntry::Invoke(
    std::span<const Value> arguments) const {
  const auto& version = storage_->version;
  auto instance = NativeInstance::Borrow(*version.state().AsObject());
  NativeExecution execution(instance);
  return InvokeNativeEntry(execution, version.function(), arguments);
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
      Value::Object(target->state), target->function));
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

void NativeDispatch::TraceMetadata(NativeMetadataTracer& tracer) const {
  if (current_ != nullptr) {
    for (const auto& [declaration, nominal] : current_->type_identities.classes) {
      tracer.Mark(nominal);
    }
    for (const auto& [declaration, interface] : current_->type_identities.interfaces) {
      tracer.Mark(interface);
    }
  }
}

HeapObject& NativeDispatch::current_state() const noexcept {
  return *current_->state.AsObject();
}

std::uint64_t NativeDispatch::revision() const noexcept {
  return current_ == nullptr ? 0 : Heap::NativeProgram(*current_->state.AsObject()).image->revision;
}

std::expected<NativePreparedPublication, NativePublicationError> NativeDispatch::Prepare(
    std::uint64_t base_revision,
    std::shared_ptr<const NativeImage> image) {
  const auto started = std::chrono::steady_clock::now();
  if (closed_ || isolate_->native_control().IsStopRequested()) {
    return std::unexpected(NativePublicationError{
        .status = NativePublicationStatus::Stopped,
        .message = "native publication is stopped",
        .declaration = {},
    });
  }
  if (base_revision != revision()) {
    return std::unexpected(NativePublicationError{
        .status = NativePublicationStatus::Stale,
        .message = "native publication baseline is stale",
        .declaration = {},
    });
  }
  if (image->native_mode != static_cast<std::uint32_t>(NativeProgramMode::Reloadable) ||
      image->revision != base_revision + 1) {
    return std::unexpected(NativePublicationError{
        .message = "reloadable native image must advance the publication revision by one",
        .declaration = {},
    });
  }
  auto snapshot = std::make_unique<NativeDispatchSnapshot>(*isolate_, Value::Null());
  const auto& description = *image;
  const auto* previous_image =
      current_ == nullptr ? nullptr : Heap::NativeProgram(*current_->state.AsObject()).image;
  if (previous_image != nullptr) {
    const auto compatible = CheckNativeNominalContracts(*previous_image, description);
    if (!compatible) {
      const auto& error = compatible.error();
      return std::unexpected(NativePublicationError{
          .status = NativePublicationStatus::RequiresReload,
          .message = error.message,
          .declaration = error.declaration,
      });
    }
  }
  if (const auto indexed = snapshot->IndexFunctions(description, current_.get()); !indexed) {
    return std::unexpected(indexed.error());
  }
  snapshot->IndexExports(description);
  if (const auto globals = snapshot->PrepareGlobals(*isolate_, description, current_.get());
      !globals) {
    return std::unexpected(globals.error());
  }
  if (current_ != nullptr) {
    snapshot->type_identities = current_->type_identities;
  }
  const auto& source_image = *image;
  const auto created = NativeInstance::Instantiate(*isolate_, source_image, std::move(image),
                                                   &snapshot->type_identities);
  if (!created) {
    return std::unexpected(NativePublicationError{
        .status = NativePublicationStatus::InstantiationFailed,
        .message = created.error().message,
        .declaration = {},
    });
  }
  const auto state = (*created)->state().front();
  snapshot->IndexLayouts(description, **created);
  snapshot->state = state;
  snapshot->root.Attach(isolate_->heap(), &snapshot->state, 1);
  Heap::NativeProgram(*state.AsObject()).dispatch = this;
  return NativePreparedPublication(*this, base_revision, std::move(snapshot),
                                   std::chrono::steady_clock::now() - started);
}

std::expected<NativePublicationTimings, NativePublicationError> NativeDispatch::Publish(
    NativePreparedPublication&& candidate) noexcept {
  using Clock = std::chrono::steady_clock;
  const auto started = Clock::now();
  NativePublicationTimings timings;
  timings.instantiation = candidate.preparation_;
  if (closed_ || isolate_->native_control().IsStopRequested()) {
    return std::unexpected(NativePublicationError{
        .status = NativePublicationStatus::Stopped,
        .message = "native publication is stopped",
        .declaration = {},
    });
  }
  if (candidate.owner_ != this || candidate.base_revision_ != revision()) {
    return std::unexpected(NativePublicationError{
        .status = NativePublicationStatus::Stale,
        .message = "native publication baseline is stale",
        .declaration = {},
    });
  }
  if (!isolate_->native_control().CanPublish()) {
    return std::unexpected(NativePublicationError{
        .status = NativePublicationStatus::Busy,
        .message = "waiting for host borrows or initialization to finish",
        .declaration = {},
    });
  }
  auto& heap = isolate_->heap();
  // 完成正在进行的增量轮次后重新追踪当前根，弱登记中的死亡实例不得阻挡结构修改。
  heap.FinishCollection();
  heap.Collect();
  const auto materializing = Clock::now();
  auto materializations = PrepareMaterializations();
  if (!materializations) {
    return std::unexpected(NativePublicationError{
        .status = NativePublicationStatus::InstantiationFailed,
        .message = "out of memory while materializing active native objects",
        .declaration = {},
    });
  }
  timings.materialization = Clock::now() - materializing;
  std::vector<NativeRetainedFunction> retained;
  heap.AppendRetainedNativeFunctions(retained);
  AppendExecutionFunctions(retained);
  const auto& image = *Heap::NativeProgram(*candidate.snapshot_->state.AsObject()).image;
  auto required = RetainedNativeDeclarations(retained);
  heap.AppendRetainedNativeTypes(required);
  if (const auto checked = CheckRetainedNativeTypes(required, image); !checked) {
    const auto& error = checked.error();
    return std::unexpected(NativePublicationError{
        .status = NativePublicationStatus::NeedsCompilation,
        .message = error.message,
        .declaration = error.declaration,
        .dependency = error.dependency,
    });
  }
  const auto* previous_image =
      current_ == nullptr ? nullptr : Heap::NativeProgram(*current_->state.AsObject()).image;
  if (const auto checked = CheckRetainedNativeContracts(retained, previous_image, image);
      !checked) {
    const auto& error = checked.error();
    return std::unexpected(NativePublicationError{
        .status = error.status == NativeContractStatus::NeedsCompilation
                      ? NativePublicationStatus::NeedsCompilation
                      : NativePublicationStatus::RequiresReload,
        .message = error.message,
        .declaration = error.declaration,
        .dependency = error.dependency,
    });
  }
  timings.validation = Clock::now() - started - timings.materialization;
  const auto preparing = Clock::now();
  auto migrations = PrepareMigrations(*candidate.snapshot_);
  if (!migrations) {
    return std::unexpected(migrations.error());
  }
  // 所有可失败准备均已完成。规范检查点已保存各帧的原版本值，
  // 包括等待已进入调用返回结果的续延。
  timings.state_preparation = Clock::now() - preparing;
  const auto deoptimizing = Clock::now();
  for (auto& materialization : *materializations) {
    timings.materialized_objects += materialization->object_count();
    materialization->Commit();
  }
  DeoptimizeExecutions();
  timings.deoptimization = Clock::now() - deoptimizing;
  const auto committing = Clock::now();
  for (const auto& migration : *migrations) {
    migration->Commit();
  }
  auto committed = std::move(candidate);
  committed.snapshot_->preparation.Release();
  current_ = std::move(committed.snapshot_);
  timings.commit = Clock::now() - committing;
  return timings;
}

std::expected<std::vector<std::unique_ptr<InstanceMigration>>, NativePublicationError>
NativeDispatch::PrepareMigrations(const NativeDispatchSnapshot& candidate) {
  auto& heap = isolate_->heap();
  std::vector<Value> objects;
  for (const auto& [type, layout] : candidate.classes) {
    heap.AppendInstances(*type, objects);
  }
  const RootRange roots(heap, objects.data(), objects.size(), RootOwner::Runtime);
  std::vector<std::unique_ptr<InstanceMigration>> migrations;
  migrations.reserve(objects.size());
  for (const auto value : objects) {
    auto& object = *value.AsObject();
    const auto* previous = Heap::ClassOf(object);
    const auto* layout = candidate.classes.at(&previous->nominal_type());
    const auto declarations = layout->field_declarations();
    auto& old_fields = Heap::InstanceFields(object);
    const auto old_states = old_fields.states();
    const auto old_declarations = previous->field_declarations();
    for (std::size_t index = 0; index < declarations.size(); ++index) {
      const auto field = declarations.subspan(index).front();
      if (field != DeclarationId{} && candidate.field_initializers.at(field) == kNoNativeIndex &&
          (index >= old_states.size() || old_declarations.subspan(index).front() != field ||
           old_states.subspan(index).front() != FieldState::Ready)) {
        return std::unexpected(NativePublicationError{
            .status = NativePublicationStatus::RequiresReload,
            .message = "a live instance has no initialized value or default for field " +
                       std::to_string(field.value),
            .declaration = field,
        });
      }
    }
    auto migration = heap.PrepareInstanceMigration(object, *layout);
    if (migration == nullptr) {
      return std::unexpected(NativePublicationError{
          .status = NativePublicationStatus::InstantiationFailed,
          .message = "out of memory preparing instance field blocks",
          .declaration = {},
      });
    }
    migrations.push_back(std::move(migration));
  }
  return migrations;
}

std::vector<DeclarationId> NativeDispatch::RetainedDeclarations() {
  auto& heap = isolate_->heap();
  heap.FinishCollection();
  heap.Collect();
  std::vector<NativeRetainedFunction> functions;
  heap.AppendRetainedNativeFunctions(functions);
  AppendExecutionFunctions(functions);
  auto declarations = RetainedNativeDeclarations(functions);
  heap.AppendRetainedNativeTypes(declarations);
  return declarations;
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
  const auto found = current_->exports.find(ExportName{.module = module_uri, .name = name});
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
  const auto status = static_cast<ExecutionStatus>(instance.image()
                                                       .functions.values()
                                                       .subspan(entry.initializer)
                                                       .front()
                                                       .entry(&execution, nullptr, 0, &result));
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

void NativeDispatch::RegisterExecution(NativeExecution& execution) noexcept {
  execution.version_next_ = executions_;
  execution.version_previous_link_ = &executions_;
  if (executions_ != nullptr) {
    executions_->version_previous_link_ = &execution.version_next_;
  }
  executions_ = &execution;
}

void NativeDispatch::UnregisterExecution(NativeExecution& execution) noexcept {
  *execution.version_previous_link_ = execution.version_next_;
  if (execution.version_next_ != nullptr) {
    execution.version_next_->version_previous_link_ = execution.version_previous_link_;
  }
}

void NativeDispatch::AppendExecutionFunctions(
    std::vector<NativeRetainedFunction>& functions) const {
  for (const auto* execution = executions_; execution != nullptr;
       execution = execution->version_next_) {
    const auto* current = execution->current_frame();
    for (const auto* frame = current; frame != nullptr; frame = frame->previous) {
      const auto* checkpoint = frame->checkpoint;
      const auto waiting = checkpoint == nullptr
                               ? frame != current
                               : checkpoint->phase == NativeCheckpointPhase::AwaitingResult;
      functions.push_back({
          .image = frame->image,
          .function = frame->position.function,
          .instruction = frame->position.instruction,
          .phase =
              waiting ? NativeDependencyPhase::AwaitingResult : NativeDependencyPhase::Instruction,
      });
    }
  }
}

void NativeDispatch::DeoptimizeExecutions() noexcept {
  for (auto* execution = executions_; execution != nullptr; execution = execution->version_next_) {
    for (auto* frame = execution->current_frame_; frame != nullptr; frame = frame->previous) {
      auto* checkpoint = frame->checkpoint;
      if (checkpoint != nullptr && checkpoint->tier == NativeCodeTier::Optimized) {
        checkpoint->tier = NativeCodeTier::Baseline;
        ++deoptimized_frames_;
      }
    }
  }
}

const ClassLayout& NativeDispatch::CurrentLayout(const NominalType& type) const noexcept {
  return *current_->classes.at(&type);
}

std::expected<Value, ExecutionStatus> NativeDispatch::LoadField(NativeExecution& execution,
                                                                HeapObject& object,
                                                                DeclarationId field) {
  const auto slot = Heap::ClassOf(object)->FieldIndex(field);
  if (!slot) {
    return std::unexpected(ExecutionStatus::TargetDeleted);
  }
  return LoadFieldAt(execution, object, field, *slot);
}

std::expected<Value, ExecutionStatus> NativeDispatch::LoadFieldAt(NativeExecution& execution,
                                                                  HeapObject& object,
                                                                  DeclarationId field,
                                                                  std::uint32_t slot) {
  auto& storage = Heap::InstanceFields(object);
  auto& state = storage.states().subspan(slot).front();
  if (state == FieldState::Ready) {
    return storage.values().subspan(slot).front();
  }
  if (state == FieldState::Initializing) {
    return std::unexpected(ExecutionStatus::CyclicInitialization);
  }
  const auto initializer = current_->field_initializers.at(field);
  if (initializer == kNoNativeIndex) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const NativeInitializationScope initialization(isolate_->native_control());
  const auto anchor = Value::Object(&object);
  const RootRange root(isolate_->heap(), &anchor, 1, RootOwner::Runtime);
  auto instance = NativeInstance::Borrow(*current_->state.AsObject());
  const NativeInstanceScope scope(execution, instance);
  state = FieldState::Initializing;
  NativeValue result;
  const auto status = static_cast<ExecutionStatus>(instance.image()
                                                       .functions.values()
                                                       .subspan(initializer)
                                                       .front()
                                                       .entry(&execution, nullptr, 0, &result));
  if (status != ExecutionStatus::Returned) {
    state = FieldState::Uninitialized;
    return std::unexpected(status);
  }
  const auto value = DecodeNativeValue(result);
  storage.values().subspan(slot).front() = value;
  state = FieldState::Ready;
  isolate_->heap().WriteBarrier(object, value);
  return value;
}

std::expected<void, ExecutionStatus> NativeDispatch::StoreField(HeapObject& object,
                                                                DeclarationId field,
                                                                Value value) noexcept {
  const auto slot = Heap::ClassOf(object)->FieldIndex(field);
  if (!slot) {
    return std::unexpected(ExecutionStatus::TargetDeleted);
  }
  return StoreFieldAt(object, *slot, value);
}

std::expected<void, ExecutionStatus> NativeDispatch::StoreFieldAt(HeapObject& object,
                                                                  std::uint32_t slot,
                                                                  Value value) noexcept {
  auto& storage = Heap::InstanceFields(object);
  auto& state = storage.states().subspan(slot).front();
  if (state == FieldState::Initializing) {
    return std::unexpected(ExecutionStatus::CyclicInitialization);
  }
  storage.values().subspan(slot).front() = value;
  state = FieldState::Ready;
  isolate_->heap().WriteBarrier(object, value);
  return {};
}

NativeFieldCache& NativeProgramState::field_cache(std::uint64_t access) {
  const auto position = DecodeNativeTracePosition(access);
  return field_caches.at(position.function).at(position.instruction);
}

std::optional<std::uint32_t> NativeFieldCache::Resolve(const ClassLayout& layout,
                                                       DeclarationId field) noexcept {
  const auto* nominal = &layout.nominal_type();
  if (type == nominal && revision == layout.revision()) {
    return slot;
  }
  const auto resolved = layout.FieldIndex(field);
  if (resolved) {
    type = nominal;
    revision = layout.revision();
    slot = *resolved;
  }
  return resolved;
}

std::expected<Value, ExecutionStatus> NativeDispatch::LoadField(NativeExecution& execution,
                                                                HeapObject& object,
                                                                DeclarationId field,
                                                                NativeFieldCache& cache) {
  const auto slot = cache.Resolve(*Heap::ClassOf(object), field);
  if (!slot) {
    return std::unexpected(ExecutionStatus::TargetDeleted);
  }
  return LoadFieldAt(execution, object, field, *slot);
}

std::expected<void, ExecutionStatus> NativeDispatch::StoreField(HeapObject& object,
                                                                DeclarationId field,
                                                                Value value,
                                                                NativeFieldCache& cache) noexcept {
  const auto slot = cache.Resolve(*Heap::ClassOf(object), field);
  if (!slot) {
    return std::unexpected(ExecutionStatus::TargetDeleted);
  }
  return StoreFieldAt(object, *slot, value);
}

extern "C" std::uint8_t vela_native_v3_load_field(NativeExecution* execution,
                                                  HeapObject* object,
                                                  std::uint64_t field,
                                                  std::uint64_t access,
                                                  NativeValue* result) {
  auto& state = Heap::NativeProgram(*execution->instance().state().front().AsObject());
  const auto loaded = state.dispatch->LoadField(*execution, *object, DeclarationId{field},
                                                state.field_cache(access));
  if (!loaded) {
    return static_cast<std::uint8_t>(loaded.error());
  }
  *result = EncodeNativeValue(*loaded);
  return static_cast<std::uint8_t>(ExecutionStatus::Returned);
}

extern "C" std::uint8_t vela_native_v3_store_field(NativeExecution* execution,
                                                   HeapObject* object,
                                                   std::uint64_t field,
                                                   std::uint64_t access,
                                                   const NativeValue* value) noexcept {
  auto& state = Heap::NativeProgram(*execution->instance().state().front().AsObject());
  const auto stored = state.dispatch->StoreField(
      *object, DeclarationId{field}, DecodeNativeValue(*value), state.field_cache(access));
  return static_cast<std::uint8_t>(stored ? ExecutionStatus::Returned : stored.error());
}

extern "C" std::uint8_t vela_native_v7_store_materialized_field(NativeExecution* execution,
                                                                HeapObject* object,
                                                                std::uint64_t field,
                                                                const NativeValue* value) noexcept {
  auto& state = Heap::NativeProgram(*execution->instance().state().front().AsObject());
  const auto stored =
      state.dispatch->StoreField(*object, DeclarationId{field}, DecodeNativeValue(*value));
  return static_cast<std::uint8_t>(stored ? ExecutionStatus::Returned : stored.error());
}

}  // namespace vela::runtime
