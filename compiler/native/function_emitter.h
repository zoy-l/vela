#pragma once

#include <absl/container/flat_hash_map.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "base/builtin.h"
#include "base/execution_status.h"
#include "base/layout_description.h"
#include "base/native_program_mode.h"
#include "compiler/ir/ir.h"
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
};

enum class NativeEntryKind : std::uint8_t {
  Initial,
  Resume,
  RecoverInitial,
  RecoverResume,
  Borrowed,
};

class FunctionEmitter final {
 public:
  FunctionEmitter(llvm::Module& module,
                  const ir::Program& program,
                  llvm::Constant* image,
                  std::size_t function_index,
                  const EscapePlan& escape,
                  std::span<const GeneratedFunction> generated,
                  const NativeRootPlan& roots,
                  NativeEntryKind kind,
                  NativeCodeTier tier)
      : module_(module),
        program_(program),
        image_(image),
        escape_(escape),
        source_(escape.function),
        origins_(escape.origins),
        function_index_(function_index),
        generated_(generated),
        body_(SelectBody(generated.subspan(function_index).front(), kind)),
        kind_(kind),
        tier_(tier),
        builder_(module.getContext()),
        roots_(roots) {}

  std::expected<void, std::string> Emit();
  [[nodiscard]] std::uint64_t frame_bytes() const noexcept { return frame_bytes_; }

 private:
  llvm::Value* Load(ir::RegisterId reg);
  llvm::Value* Payload(ir::RegisterId reg);
  llvm::Value* Reference(ir::RegisterId reg);
  void Store(ir::RegisterId reg, llvm::Value* value, const SourceType& type);
  void Check(llvm::Value* condition, ExecutionStatus failure);
  void CheckStatus(llvm::Value* status);
  void Fail(llvm::Value* status);
  void SetPosition();
  void PrepareRuntimeCall();
  std::uint64_t TracePosition() const;
  void Poll();
  static llvm::Function* SelectBody(const GeneratedFunction& function, NativeEntryKind kind);
  [[nodiscard]] bool IsRecovering() const noexcept;
  [[nodiscard]] bool IsResuming() const noexcept;
  llvm::Value* ResumeState();
  void InitializeCheckpoint();
  void StoreCheckpoint();
  void InitializeDebugValues();
  void StoreDebugValues();
  llvm::Constant* VirtualGraph(std::size_t instruction);
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
  void EmitIntrinsicImport();
  void EmitComparison(const ir::Instruction& instruction);
  void EmitValueEquality(const ir::Instruction& instruction);
  llvm::Value* ByteAddress(llvm::Value* object, std::uint64_t offset);
  llvm::Value* ProgramData();
  llvm::Value* ReadHeapValue(llvm::Value* address, const SourceType& type);
  void WriteHeapValue(llvm::Value* address, ir::RegisterId reg);
  void EmitWriteBarrier(llvm::Value* object, ir::RegisterId reg);
  void EmitField(const ir::Instruction& instruction);
  void EmitReloadableField(const ir::Instruction& instruction, llvm::Value* object);
  void EmitGlobal(const ir::Instruction& instruction);
  void EmitGlobalLoad(const ir::Instruction& instruction);
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
  void EmitCall(const ir::Instruction& instruction);
  void BindArguments();
  void InitializeRecords();
  void EmitConstant(const ir::Instruction& instruction);
  std::expected<void, std::string> EmitInstructionBlock(std::size_t index);
  std::expected<void, std::string> PrepareFrame();
  [[nodiscard]] bool CanUseLeafEntry() const;
  void BindFrameStorage();
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
  llvm::Constant* image_;
  const EscapePlan& escape_;
  const ir::Function& source_;
  std::span<const std::size_t> origins_;
  std::size_t function_index_;
  std::size_t instruction_index_ = 0;
  std::vector<std::optional<std::uint32_t>> field_accesses_;
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
  llvm::Value* checkpoint_values_ = nullptr;
  llvm::Value* recovery_storage_ = nullptr;
  llvm::Value* virtual_references_ = nullptr;
  llvm::Value* trace_frame_ = nullptr;
  llvm::Value* debug_values_ = nullptr;
  llvm::Value* field_caches_ = nullptr;
  bool leaf_entry_ = false;
  llvm::StructType* frame_type_ = nullptr;
  std::uint64_t frame_bytes_ = 0;
  std::uint64_t storage_bytes_ = 0;
  absl::flat_hash_map<std::uint32_t, llvm::Value*> imports_;
  const NativeRootPlan& roots_;
};

std::expected<NativeFunctionMetrics, std::string> EmitFunction(
    llvm::Module& module,
    const ir::Program& program,
    llvm::Constant* image,
    std::size_t function_index,
    std::span<const GeneratedFunction> generated,
    NativeDebugging debugging,
    NativeCodeTier tier = NativeCodeTier::Baseline,
    EscapeOptimization optimization = EscapeOptimization::Enabled,
    std::span<const std::optional<ir::ClassId>> borrowable = {});

std::expected<NativeFunctionMetrics, std::string> EmitBorrowedFunction(
    llvm::Module& module,
    const ir::Program& program,
    llvm::Constant* image,
    std::size_t function_index,
    const EscapePlan& plan,
    std::span<const GeneratedFunction> generated,
    NativeDebugging debugging);

}  // namespace vela::compiler::native
