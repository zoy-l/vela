#pragma once

#include <absl/container/flat_hash_map.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "base/builtin.h"
#include "base/callable.h"
#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/layout_description.h"
#include "base/native_program_mode.h"
#include "base/reflection.h"
#include "base/reflection_operation.h"
#include "compiler/ir/ir.h"
#include "compiler/native/array_analysis.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/field_access.h"
#include "compiler/native/global_analysis.h"
#include "compiler/native/memory_regions.h"
#include "compiler/native/native_frame_plan.h"
#include "compiler/native/native_roots.h"
#include "compiler/native/native_values.h"

namespace llvm {
class Function;
class Module;
}  // namespace llvm

namespace vela::compiler::native {

inline constexpr std::uint32_t kRecursiveLeafDepth = 8;
inline constexpr std::uint8_t kNativeLeafRetry = 255;

struct GeneratedFunction final {
  llvm::Function* body = nullptr;
  llvm::Function* entry = nullptr;
  llvm::Function* reflection_entry = nullptr;
  llvm::Function* bounded_reflection_body = nullptr;
  llvm::Function* resume = nullptr;
  llvm::Function* baseline_body = nullptr;
  llvm::Function* baseline_resume = nullptr;
  llvm::Function* recovery = nullptr;
  llvm::Function* resume_recovery = nullptr;
  llvm::Function* borrowed = nullptr;
  llvm::Function* leaf = nullptr;
  llvm::Function* leaf_guard = nullptr;
  bool budgeted_recursion = false;
  bool square_root = false;
  [[nodiscard]] bool HasRecovery(NativeCodeTier tier) const noexcept {
    return tier == NativeCodeTier::Optimized && recovery != nullptr;
  }
};

enum class NativeEntryKind : std::uint8_t {
  Initial,
  BoundedReflection,
  Resume,
  RecoverInitial,
  RecoverResume,
  Borrowed,
  Leaf,
};

// 同一分流的 initial、resume、leaf 和恢复入口共享不可变分析结果。
struct FunctionEmissionFacts final {
  NativeCodePlacement placement;
  const FieldTargets* field_targets = nullptr;
  std::size_t temporary_count = 0;
  std::size_t suspension_capacity = 0;
  std::vector<std::optional<std::uint32_t>> field_accesses;
  std::vector<FieldKindWrite> field_kind_writes;
  std::vector<std::optional<CallableTarget>> direct_calls;
  std::vector<std::optional<reflection::Operation>> reflection_operations;
  ArrayEmissionFacts arrays;
  const GlobalAccessFacts* global_access = nullptr;
};

class FunctionEmitter final {
 public:
  FunctionEmitter(llvm::Module& module,
                  const ir::Program& program,
                  const NativeCallPlan& calls,
                  llvm::Constant* image,
                  std::size_t function_index,
                  const EscapePlan& escape,
                  std::span<const GeneratedFunction> generated,
                  const NativeRootPlan& roots,
                  const FunctionEmissionFacts& facts,
                  NativeEntryKind kind,
                  NativeCodeTier tier)
      : module_(module),
        program_(program),
        calls_(calls),
        image_(image),
        escape_(escape),
        source_(escape.function.get()),
        origins_(escape.origins),
        function_index_(function_index),
        placement_(facts.placement),
        field_targets_(*facts.field_targets),
        temporary_count_(facts.temporary_count),
        suspension_capacity_(facts.suspension_capacity),
        field_accesses_(facts.field_accesses),
        field_kind_writes_(facts.field_kind_writes),
        direct_calls_(facts.direct_calls),
        generated_(generated),
        body_(SelectBody(generated[function_index], kind)),
        kind_(kind),
        tier_(tier),
        builder_(module.getContext()),
        array_access_(facts.arrays),
        global_access_(*facts.global_access),
        roots_(roots),
        reflection_operations_(facts.reflection_operations),
        closed_field_declarations_(source_.instructions.size()) {}

  std::expected<void, std::string> Emit();
  const NativeFrameLayout& PlanFrame();
  [[nodiscard]] std::uint64_t frame_bytes() const noexcept { return frame_bytes_; }

 private:
  llvm::MDNode* MemoryTag(MemoryRegion region);
  llvm::LoadInst* LoadMemory(llvm::Type* type, llvm::Value* address, MemoryRegion region);
  llvm::StoreInst* StoreMemory(llvm::Value* value, llvm::Value* address, MemoryRegion region);
  llvm::Value* Load(ir::RegisterId reg);
  bool HasRangeProof(std::span<const std::uint8_t> proofs) const;
  const ir::Instruction& CanonicalInstruction(std::size_t origin) const;
  llvm::Value* Payload(ir::RegisterId reg);
  llvm::Value* Reference(ir::RegisterId reg);
  void Store(ir::RegisterId reg, llvm::Value* value, const SourceType& type);
  void Check(llvm::Value* condition, ExecutionStatus failure);
  void CheckReference(llvm::Value* reference);
  struct LeafFailure final {
    llvm::Value* status;
    llvm::Value* position;
    llvm::Value* roots;
    std::size_t count;
  };
  llvm::Value* EmitLeafFailure(LeafFailure failure);
  void CheckStatus(llvm::Value* status);
  void Fail(llvm::Value* status);
  void PrepareTraceFrame();
  void InitializeTraceFrame(llvm::Value* frame);
  void PrepareInlineRuntimeCall();
  llvm::Value* CallRuntimeBuiltin(const ir::Instruction& instruction);
  void CacheGlobals();
  void CacheInitializedGlobals(std::size_t callee);
  bool IsGlobalInitialized(std::size_t index) const;
  bool IsGlobalInitialized() const { return IsGlobalInitialized(instruction_index_); }
  void MarkGlobalInitialized(std::uint32_t slot);
  llvm::Value* IsGlobalReady(llvm::Value* address);
  void StoreGlobalReady(llvm::Value* address);
  llvm::Value* GlobalValueAddress(std::uint32_t slot);
  std::size_t PlanFixedArrayStorage();
  void CacheFixedArrayStorage();
  void StoreFixedArrayStorage(ir::RegisterId reg, llvm::Value* value);
  enum class ArrayStorageField : std::uint8_t { Data, Length, Mutation };
  std::array<llvm::Type*, 3> ArrayStorageTypes();
  llvm::Value* LoadArrayStorageField(llvm::Value* array, ArrayStorageField field);
  llvm::Value* ReadArrayStorage(llvm::Value* array, ir::RegisterId reg, ArrayStorageField field);
  void SetPosition();
  void PrepareRuntimeCall();
  enum class CallEntry : std::uint8_t { Body, Borrowed, Leaf, Slot };
  CallEntry SelectCallEntry(CallableTarget target, std::size_t instruction) const;
  void PrepareObservableCall();
  void PrepareCallCheckpoint();
  void MarkClosedInvariantCall(llvm::CondBrInst& branch) const;
  llvm::Value* ControlState();
  llvm::Value* ProgramData();
  llvm::Value* LoadSafepointRequests();
  enum class HostBorrow : std::uint8_t { Enter, Exit };
  void EmitHostBoundary(HostBorrow change);
  void EnterHostFrame(llvm::Value* depth);
  void RestoreHostFrame();
  void StoreExecutionState(Builder& builder, llvm::Value* frame, llvm::Value* depth);
  void ReleaseBorrowedHostFrame();
  llvm::Value* EmitHostPointer(llvm::Value* wrapper, const SourceType& type);
  llvm::Value* EmitDirectHostCall(const ir::Function& callee,
                                  llvm::Value* entries,
                                  std::span<llvm::Value* const> arguments);
  llvm::Value* EmitHostFunctionCall(const SourceType& result_type,
                                    llvm::Value* target,
                                    std::span<llvm::Value* const> arguments);
  void ReleaseHostHandles(llvm::Value* context);
  void InitializeNativeStringView(llvm::Value* string, llvm::Value* storage);
  llvm::Value* EmitNativeStringArgument(llvm::Value* string,
                                        std::size_t parameter,
                                        llvm::Value* kind);
  bool CanInlineImportCall(const ir::Function& callee) const;
  llvm::BasicBlock* EmitInlineImportCall(const ir::Instruction& instruction,
                                         CallableTarget selected,
                                         std::span<llvm::Value* const> arguments);
  std::uint64_t TracePosition() const;
  llvm::Value* Poll();
  bool IsBoundaryPollRequired(std::size_t index) const;
  bool HasInlineRuntimePath(std::size_t index) const;
  void CacheRuntimeCalls();
  void PollAfterRuntimeCall();
  static llvm::Function* SelectBody(const GeneratedFunction& function, NativeEntryKind kind);
  [[nodiscard]] bool IsRecovering() const noexcept;
  [[nodiscard]] bool IsResuming() const noexcept;
  llvm::Value* ResumeState();
  void InitializeCheckpoint();
  void PrepareCheckpoint();
  void StoreRecoveryDescription();
  bool HasRecovery() const;
  void StoreCheckpoint();
  void InitializeDebugValues();
  void StoreDebugValues();
  std::vector<std::uint8_t> debug_live_;
  llvm::Constant* VirtualGraph(std::size_t instruction);
  void Recover();
  void RecoverIfInvalidated();
  void EmitRecoveryDispatch();
  void GuardResume();
  void BeginConstruction();
  llvm::Value* InitializationsAddress(Builder& builder);
  void ExitInitializations(Builder& builder, llvm::Value* count);
  void EndConstruction();
  void AbortConstructions();
  void Finish(llvm::Value* status);
  llvm::SwitchInst* ResumeDispatch(llvm::Value* instruction);
  void EmitInteger(const ir::Instruction& instruction);
  void EmitDouble(const ir::Instruction& instruction);
  void EmitNumericConversion(const ir::Instruction& instruction, Builtin builtin);
  llvm::Value* SquareRootCondition(llvm::Value& entries, llvm::Value* operand);
  void EmitIntrinsicImport();
  void EmitComparison(const ir::Instruction& instruction);
  void EmitValueEquality(const ir::Instruction& instruction);
  llvm::Value* ByteAddress(llvm::Value* object, std::uint64_t offset);
  struct StoredValueAddress final {
    llvm::Value* payload;
    llvm::Value* kind;
    MemoryRegion payload_region;
    MemoryRegion kind_region;
  };
  struct StoredValue final {
    const SourceType* type;
    llvm::Value* value;
    llvm::Value* encoded;
  };
  StoredValue LoadStoredValue(ir::RegisterId reg);
  void StoreRootValue(ir::RegisterId reg, llvm::Value* address);
  llvm::Value* EncodeStoredValue(StoredValue& value);
  StoredValueAddress RecordAddress(llvm::Value* address, MemoryRegion region);
  StoredValueAddress FieldAddress(llvm::Value* storage,
                                  llvm::Value& index,
                                  llvm::Value* kind_offset);
  StoredValueAddress ObjectFieldAddress(llvm::Value* object, std::uint32_t index);
  StoredValueAddress DirectFieldAddress(llvm::Value* storage,
                                        std::uint32_t slot,
                                        std::uint32_t count);
  llvm::Value* ReadStoredValue(StoredValueAddress address, const SourceType& type);
  void WriteStoredValue(StoredValueAddress address,
                        StoredValue& stored,
                        FieldKindWrite kind_write = FieldKindWrite::Required);
  llvm::Value* ReadHeapValue(llvm::Value* address, const SourceType& type);
  void EmitWriteBarrier(llvm::Value* object, StoredValue& stored);
  void EmitFieldAccess(const ir::Instruction& instruction,
                       llvm::Value* object,
                       const StoredValueAddress& address);
  llvm::BasicBlock* EmitFieldReadiness(llvm::Value* storage,
                                       const ir::Instruction& instruction,
                                       llvm::Value* slot,
                                       std::optional<std::uint32_t> count,
                                       llvm::BasicBlock* unavailable);
  void EmitField(const ir::Instruction& instruction);
  void EmitReloadableField(const ir::Instruction& instruction, llvm::Value* object);
  void EmitOptimizedField(const ir::Instruction& instruction, llvm::Value* object);
  void EmitGlobal(const ir::Instruction& instruction);
  void EmitGlobalLoad(const ir::Instruction& instruction);
  void EmitReloadableGlobal(const ir::Instruction& instruction);
  void EmitGlobalStore(const ir::Instruction& instruction);
  const layout::Type& ArrayElement(const ir::Instruction& instruction) const;
  llvm::Type* ElementStorage(const layout::Type& element);
  void WriteElement(llvm::Value* address, const layout::Type& element, StoredValue& stored);
  void EmitIndex(const ir::Instruction& instruction);
  void EmitArrayBuiltin(const ir::Instruction& instruction, Builtin builtin);
  void EmitStringCodePointAt(const ir::Instruction& instruction);
  void EmitAppendCodePoint(const ir::Instruction& instruction);
  void EmitMapBuiltin(const ir::Instruction& instruction, Builtin builtin);
  void EmitLocatedMapBuiltin(const ir::Instruction& instruction, Builtin builtin, llvm::Value* map);
  std::expected<void, std::string> EmitBuiltin(const ir::Instruction& instruction);
  std::expected<void, std::string> EmitReference(const ir::Instruction& instruction);
  llvm::Value* Encode(ir::RegisterId reg);
  llvm::Value* Pack(llvm::Value* encoded, std::size_t index);
  enum class ArgumentPrefix : std::uint8_t { None, Implicit };
  void PackArguments(ir::RegisterId first,
                     std::size_t count,
                     ArgumentPrefix prefix = ArgumentPrefix::None);
  void Unpack(ir::RegisterId reg);
  void CompleteCall(llvm::Value* status, ir::RegisterId result);
  void EmitRuntimeBuiltin(const ir::Instruction& instruction);
  llvm::Value* EmitTypeTest(const ir::Instruction& instruction);
  void EmitReflectedField(const ir::Instruction& instruction, reflection::Operation operation);
  void EmitReflectedCall(const ir::Instruction& instruction, reflection::Operation operation);
  const reflection::Member* BoundReflectedMember(const ir::Instruction& instruction,
                                                 reflection::Operation operation) const;
  llvm::Value* EmitReflectedCallTarget(const ir::Instruction& instruction,
                                       reflection::Operation operation,
                                       llvm::Value* entry,
                                       std::span<llvm::Value* const> arguments);
  void EmitReflectedLookup(const ir::Instruction& instruction, reflection::Operation operation);
  void CacheReflectionMembers();
  llvm::Value* ReflectionCacheAddress(std::size_t offset);
  llvm::Value* IsReflectionCacheReady(llvm::Value* epoch);
  llvm::Value* IsReflectionEpochCurrent(llvm::Value& epoch_address, llvm::Value* epoch_observed);
  llvm::Value* IsReflectionReceiver(ir::RegisterId receiver);
  void GuardReflectionLayout(llvm::Value& receiver,
                             llvm::Value* receiver_layout_id,
                             llvm::BasicBlock& slow);
  void StoreRoots(std::size_t index);
  void ClearRoots(std::size_t index);
  llvm::Value* RootAddress(std::size_t slot);
  void ClearRoot(std::size_t slot);
  std::expected<void, std::string> EmitInstruction(const ir::Instruction& instruction);
  std::optional<CallableTarget> CallTarget(std::size_t index) const;
  bool IsLeafCall(std::optional<CallableTarget> target, std::size_t instruction) const;
  void StoreCallResult(ir::RegisterId reg, const SourceType& type, llvm::Value* address);
  void EmitCall(const ir::Instruction& instruction, CallableTarget selected);
  void EmitLeafCall(const ir::Instruction& instruction,
                    CallableTarget selected,
                    std::span<llvm::Value* const> arguments);
  llvm::BasicBlock* EmitSquareRootCall(const ir::Instruction& instruction, std::uint32_t target);
  std::vector<llvm::Value*> CallArguments(const ir::Instruction& instruction,
                                          CallableTarget selected,
                                          std::optional<std::size_t> borrowed);
  void BindArguments();
  void InitializeRecords();
  void EmitConstant(const ir::Instruction& instruction);
  std::expected<void, std::string> EmitInstructionBlock(std::size_t index);
  void PrepareFrame();
  template <typename Address>
  void BindFrameSlots(const Address& address);
  std::size_t PlanAuxiliaryStorage();
  [[nodiscard]] bool CanUseLeafEntry() const;
  [[nodiscard]] bool CanUseSynchronousStackFrame() const;
  [[nodiscard]] bool CanDeferRoots() const;
  [[nodiscard]] bool CanDelayRootRegistration() const;
  [[nodiscard]] bool CanExecuteWithoutCollection(std::size_t index) const;
  void BindFrameStorage();
  llvm::Value* HasStackBudget(llvm::Value* depth, std::uint64_t reserved_bytes = 0);
  void EmitFrameEntry();
  void EmitTraceFrame(llvm::Value* frame, Builder& cleanup, llvm::Value* depth);
  void EmitReflection(const ir::Instruction& instruction);
  llvm::Value* LinkStackRoots();
  void CacheImports();
  void BorrowHostExecution();
  void CacheConstants();
  void CacheNativeStringViews();
  void CacheMapLookups();
  std::size_t PlanStringViewStorage();
  std::size_t PlanMapLookupStorage();
  llvm::Value* AllocateAuxiliary(std::size_t bytes);
  void CacheFields();
  void EmitImport();
  llvm::Value* Output();
  void EmitAsyncFailure(llvm::Value* status);
  void EmitAsyncReturn(const ir::Instruction& instruction);
  void EmitAsyncInstruction(const ir::Instruction& instruction);
  void EmitAwait(const ir::Instruction& instruction);
  void EmitResumeDispatch();
  void StoreAsyncResult();

  llvm::Module& module_;
  const ir::Program& program_;
  const NativeCallPlan& calls_;
  llvm::Value* image_;
  const EscapePlan& escape_;
  const ir::Function& source_;
  std::span<const std::size_t> origins_;
  std::size_t function_index_;
  NativeCodePlacement placement_;
  std::size_t instruction_index_ = 0;
  const FieldTargets& field_targets_;
  std::size_t temporary_count_;
  std::size_t suspension_capacity_;
  std::vector<std::uint8_t> runtime_calls_;
  const std::vector<std::optional<std::uint32_t>>& field_accesses_;
  const std::vector<FieldKindWrite>& field_kind_writes_;
  const std::vector<std::optional<CallableTarget>>& direct_calls_;
  std::span<const GeneratedFunction> generated_;
  llvm::Function* body_;
  NativeEntryKind kind_;
  NativeCodeTier tier_;
  Builder builder_;
  std::vector<llvm::Value*> slots_;
  std::vector<llvm::Value*> records_;
  std::vector<llvm::BasicBlock*> blocks_;
  llvm::BasicBlock* cleanup_ = nullptr;
  llvm::PHINode* status_ = nullptr;
  llvm::Value* arguments_ = nullptr;
  llvm::Value* host_parent_ = nullptr;
  llvm::Value* host_trace_active_ = nullptr;
  llvm::Value* entry_depth_ = nullptr;
  llvm::Value* global_owner_ = nullptr;
  llvm::Value* global_values_ = nullptr;
  llvm::Value* global_states_ = nullptr;
  llvm::Value* global_caches_ = nullptr;
  absl::flat_hash_map<std::uint32_t, llvm::Value*> global_ready_;
  absl::flat_hash_map<std::uint32_t, llvm::Value*> global_cached_values_;
  llvm::Value* result_ = nullptr;
  llvm::Value* leaf_result_ = nullptr;
  llvm::Value* instruction_slot_ = nullptr;
  llvm::Value* root_slots_ = nullptr;
  llvm::Value* stack_roots_frame_ = nullptr;
  llvm::Value* stack_roots_head_ = nullptr;
  llvm::Value* control_state_ = nullptr;
  llvm::Value* program_data_ = nullptr;
  llvm::Value* boundary_ready_ = nullptr;
  llvm::Value* frame_storage_ = nullptr;
  llvm::Value* construction_depth_ = nullptr;
  llvm::Value* checkpoint_ = nullptr;
  llvm::Constant* checkpoint_layout_ = nullptr;
  llvm::Value* checkpoint_initialized_ = nullptr;
  llvm::Value* checkpoint_values_ = nullptr;
  llvm::Value* recovery_storage_ = nullptr;
  llvm::Value* virtual_references_ = nullptr;
  llvm::Value* virtual_state_ = nullptr;
  llvm::Value* trace_frame_ = nullptr;
  llvm::Value* trace_initialized_ = nullptr;
  llvm::Value* import_frame_ = nullptr;
  llvm::Value* debug_values_ = nullptr;
  llvm::Value* field_caches_ = nullptr;
  struct DirectField final {
    llvm::Value* layout;
    ir::FieldId slot;
    std::uint32_t count;
  };
  absl::flat_hash_map<DeclarationId, DirectField> field_layouts_;
  bool leaf_entry_ = false;
  bool deferred_roots_ = false;
  bool delayed_root_registration_ = false;
  llvm::StructType* frame_type_ = nullptr;
  std::vector<std::optional<unsigned>> frame_locals_;
  unsigned temporary_field_ = 0;
  std::optional<unsigned> leaf_result_field_;
  std::optional<unsigned> auxiliary_field_;
  llvm::Value* auxiliary_storage_ = nullptr;
  std::size_t auxiliary_offset_ = 0;
  std::optional<std::size_t> auxiliary_bytes_;
  std::uint64_t frame_bytes_ = 0;
  std::uint64_t storage_bytes_ = 0;
  std::optional<NativeFrameLayout> frame_plan_;
  absl::flat_hash_map<std::uint32_t, llvm::Value*> imports_;
  absl::flat_hash_map<std::uint32_t, llvm::Value*> constants_;
  std::vector<std::optional<std::uint32_t>> string_constant_registers_;
  std::vector<std::uint32_t> native_view_constants_;
  absl::flat_hash_map<std::uint32_t, llvm::Value*> constant_string_views_;
  std::vector<llvm::Value*> map_lookups_;
  std::vector<llvm::Value*> fixed_arrays_;
  llvm::StructType* fixed_array_cache_type_ = nullptr;
  std::size_t fixed_array_cache_bytes_ = 0;
  std::vector<std::optional<std::size_t>> map_lookup_pairs_;
  ArrayEmissionFacts array_access_;
  const GlobalAccessFacts& global_access_;
  const NativeRootPlan& roots_;
  std::span<const std::optional<reflection::Operation>> reflection_operations_;
  std::vector<const reflection::Member*> reflection_members_;
  std::array<llvm::MDNode*, static_cast<std::size_t>(MemoryRegion::Count)> memory_tags_{};
  absl::flat_hash_map<std::uint64_t, llvm::MDNode*> field_payload_tags_;
  absl::flat_hash_map<std::uint64_t, llvm::MDNode*> field_kind_tags_;
  std::vector<std::optional<DeclarationId>> closed_field_declarations_;
};

struct NativeFunctionAnalysis final {
  EscapePlan escape;
  NativeRootPlan roots;
};

[[nodiscard]] NativeFunctionAnalysis PlanFunctionAnalysis(
    const ir::Program& program,
    std::size_t function_index,
    NativeDebugging debugging,
    NativeCodeTier tier,
    EscapeOptimization optimization,
    std::span<const std::uint8_t> readonly_globals,
    std::span<const std::optional<ir::ClassId>> borrowable);

std::expected<NativeFunctionMetrics, std::string> EmitFunction(
    llvm::Module& module,
    const ir::Program& program,
    const NativeCallPlan& calls,
    const FieldTargets& field_targets,
    ArrayEmissionFacts array_access,
    const GlobalAccessFacts& global_access,
    llvm::Constant* image,
    std::size_t function_index,
    const NativeFunctionAnalysis& analysis,
    std::span<const GeneratedFunction> generated,
    std::vector<std::size_t>& body_dependencies,
    NativeDebugging debugging,
    NativeCodePlacement placement,
    NativeCodeTier tier);

std::expected<NativeFunctionMetrics, std::string> EmitBorrowedFunction(
    llvm::Module& module,
    const ir::Program& program,
    const NativeCallPlan& calls,
    const FieldTargets& field_targets,
    ArrayEmissionFacts array_access,
    const GlobalAccessFacts& global_access,
    llvm::Constant* image,
    std::size_t function_index,
    const EscapePlan& plan,
    std::span<const GeneratedFunction> generated,
    NativeDebugging debugging,
    NativeCodePlacement placement);

}  // namespace vela::compiler::native
