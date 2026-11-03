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
#include "compiler/ir/ir.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/native_roots.h"
#include "compiler/native/native_values.h"

namespace llvm {
class Function;
class Module;
}  // namespace llvm

namespace vela::compiler::native {

struct GeneratedFunction final {
  llvm::Function* body = nullptr;
  llvm::Function* entry = nullptr;
  llvm::Function* resume = nullptr;
  llvm::Function* baseline_body = nullptr;
  llvm::Function* baseline_resume = nullptr;
  llvm::Function* recovery = nullptr;
  llvm::Function* resume_recovery = nullptr;
  llvm::Function* borrowed = nullptr;
  llvm::Function* leaf = nullptr;
  llvm::Function* leaf_guard = nullptr;
  [[nodiscard]] bool HasRecovery(NativeCodeTier tier) const noexcept {
    return tier == NativeCodeTier::Optimized && recovery != nullptr;
  }
};

enum class NativeEntryKind : std::uint8_t {
  Initial,
  Resume,
  RecoverInitial,
  RecoverResume,
  Borrowed,
  Leaf,
};

// 同一分流的 initial、resume、leaf 和恢复入口共享不可变分析结果。
struct FunctionEmissionFacts final {
  std::vector<std::optional<std::uint32_t>> field_accesses;
  std::vector<std::optional<CallableTarget>> direct_calls;
  std::vector<std::uint64_t> array_element_bits;
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
        field_accesses_(facts.field_accesses),
        direct_calls_(facts.direct_calls),
        generated_(generated),
        body_(SelectBody(generated.subspan(function_index).front(), kind)),
        kind_(kind),
        tier_(tier),
        builder_(module.getContext()),
        array_element_bits_(facts.array_element_bits),
        roots_(roots) {}

  std::expected<void, std::string> Emit();
  [[nodiscard]] std::uint64_t frame_bytes() const noexcept { return frame_bytes_; }

 private:
  // 这些存储区域互不覆盖；装箱 Value 共用别名域，拆分字段的载荷与标签各自独立。
  enum class MemoryRegion : std::uint8_t {
    HeapHeader,
    ClassLayout,
    FieldCache,
    InstanceHeader,
    FieldStates,
    Values,
    Elements,
    GlobalCache,
    Roots,
    GcState,
    FieldPayloads,
    FieldKinds,
    Count,
  };
  std::array<llvm::MDNode*, static_cast<std::size_t>(MemoryRegion::Count)> memory_tags_{};
  llvm::MDNode* MemoryTag(MemoryRegion region);
  llvm::LoadInst* LoadMemory(llvm::Type* type, llvm::Value* address, MemoryRegion region);
  llvm::StoreInst* StoreMemory(llvm::Value* value, llvm::Value* address, MemoryRegion region);
  llvm::Value* Load(ir::RegisterId reg);
  llvm::Value* Payload(ir::RegisterId reg);
  llvm::Value* Reference(ir::RegisterId reg);
  void Store(ir::RegisterId reg, llvm::Value* value, const SourceType& type);
  void Check(llvm::Value* condition, ExecutionStatus failure);
  void CheckStatus(llvm::Value* status);
  void Fail(llvm::Value* status);
  void PrepareTraceFrame();
  void SetPosition();
  void PrepareRuntimeCall();
  bool IsEntrySlotRequired(std::size_t callee) const;
  void PrepareObservableCall();
  void PrepareCallCheckpoint();
  llvm::Value* ControlState();
  llvm::Value* LoadSafepointRequests();
  enum class HostBorrow : std::uint8_t { Enter, Exit };
  void EmitHostBoundary(HostBorrow change);
  llvm::Value* EmitDirectHostCall(const ir::Function& callee,
                                  llvm::Value* entries,
                                  std::span<llvm::Value* const> arguments);
  bool CanInlineImportCall(const ir::Function& callee) const;
  llvm::BasicBlock* EmitInlineImportCall(const ir::Instruction& instruction,
                                         CallableTarget selected,
                                         std::span<llvm::Value* const> arguments);
  std::uint64_t TracePosition() const;
  void Poll();
  bool IsBoundaryPollRequired(std::size_t index) const;
  bool HasInlineRuntimePath(std::size_t index) const;
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
  llvm::Constant* VirtualGraph(std::size_t instruction);
  void Recover();
  void RecoverIfInvalidated();
  void EmitRecoveryDispatch();
  void GuardResume();
  void BeginConstruction();
  void EndConstruction();
  void AbortConstructions();
  void Finish(llvm::Value* status);
  llvm::SwitchInst* ResumeDispatch(llvm::Value* instruction);
  void EmitInteger(const ir::Instruction& instruction);
  void EmitDouble(const ir::Instruction& instruction);
  void EmitNumericConversion(const ir::Instruction& instruction, Builtin builtin);
  bool CanInlineSquareRoot(const ir::Function& function) const;
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
  StoredValueAddress RecordAddress(llvm::Value* address, MemoryRegion region);
  StoredValueAddress FieldAddress(llvm::Value* storage,
                                  llvm::Value& index,
                                  llvm::Value* kind_offset);
  llvm::Value* ReadStoredValue(StoredValueAddress address, const SourceType& type);
  void WriteStoredValue(StoredValueAddress address, ir::RegisterId reg);
  llvm::Value* ReadHeapValue(llvm::Value* address, const SourceType& type);
  void WriteHeapValue(llvm::Value* address,
                      ir::RegisterId reg,
                      MemoryRegion region = MemoryRegion::Values);
  void EmitWriteBarrier(llvm::Value* object, ir::RegisterId reg);
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
  void WriteElement(llvm::Value* address, const layout::Type& element, ir::RegisterId reg);
  void EmitIndex(const ir::Instruction& instruction);
  void EmitArrayBuiltin(const ir::Instruction& instruction, Builtin builtin);
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
  void StoreRoots(std::size_t index);
  void ClearRoots(std::size_t index);
  llvm::Value* RootAddress(std::size_t slot);
  void ClearRoot(std::size_t slot);
  std::expected<void, std::string> EmitInstruction(const ir::Instruction& instruction);
  std::optional<CallableTarget> CallTarget(std::size_t index) const;
  void EmitCall(const ir::Instruction& instruction, CallableTarget selected);
  llvm::BasicBlock* EmitSquareRootCall(const ir::Instruction& instruction, std::uint32_t target);
  std::vector<llvm::Value*> CallArguments(const ir::Instruction& instruction,
                                          CallableTarget selected,
                                          std::optional<std::size_t> borrowed);
  void BindArguments();
  void InitializeRecords();
  void EmitConstant(const ir::Instruction& instruction);
  std::expected<void, std::string> EmitInstructionBlock(std::size_t index);
  std::expected<void, std::string> PrepareFrame();
  [[nodiscard]] bool CanUseLeafEntry() const;
  void BindFrameStorage();
  llvm::Value* HasStackBudget(llvm::Value* depth);
  void EmitFrameEntry();
  void CacheImports();
  void CacheFields();
  void EmitImport();
  llvm::Value* Output();
  void EmitAsyncFailure(llvm::Value* status);
  void EmitAsyncReturn(const ir::Instruction& instruction);
  std::expected<void, std::string> EmitAsyncInstruction(const ir::Instruction& instruction);
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
  std::size_t instruction_index_ = 0;
  const std::vector<std::optional<std::uint32_t>>& field_accesses_;
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
  llvm::Value* result_ = nullptr;
  llvm::Value* instruction_slot_ = nullptr;
  llvm::Value* root_slots_ = nullptr;
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
  llvm::StructType* frame_type_ = nullptr;
  std::vector<std::optional<unsigned>> frame_locals_;
  unsigned temporary_field_ = 0;
  std::uint64_t frame_bytes_ = 0;
  std::uint64_t storage_bytes_ = 0;
  absl::flat_hash_map<std::uint32_t, llvm::Value*> imports_;
  const std::vector<std::uint64_t>& array_element_bits_;
  const NativeRootPlan& roots_;
};

std::expected<NativeFunctionMetrics, std::string> EmitFunction(
    llvm::Module& module,
    const ir::Program& program,
    const NativeCallPlan& calls,
    llvm::Constant* image,
    std::size_t function_index,
    std::span<const GeneratedFunction> generated,
    std::vector<std::size_t>& body_dependencies,
    NativeDebugging debugging,
    NativeCodeTier tier = NativeCodeTier::Baseline,
    EscapeOptimization optimization = EscapeOptimization::Enabled,
    std::span<const std::optional<ir::ClassId>> borrowable = {});

std::expected<NativeFunctionMetrics, std::string> EmitBorrowedFunction(
    llvm::Module& module,
    const ir::Program& program,
    const NativeCallPlan& calls,
    llvm::Constant* image,
    std::size_t function_index,
    const EscapePlan& plan,
    std::span<const GeneratedFunction> generated,
    NativeDebugging debugging);

}  // namespace vela::compiler::native
