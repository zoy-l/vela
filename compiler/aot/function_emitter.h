#pragma once

#include <absl/container/flat_hash_map.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>
#include "base/builtin.h"
#include "base/execution_status.h"
#include "base/layout_description.h"
#include "compiler/aot/native_roots.h"
#include "compiler/aot/native_values.h"
#include "compiler/ir/ir.h"

namespace llvm {
class Function;
class Module;
}  // namespace llvm

namespace vela::compiler::aot {

struct GeneratedFunction final {
  llvm::Function* body = nullptr;
  llvm::Function* entry = nullptr;
  llvm::Function* resume = nullptr;
};

enum class NativeEntryKind : std::uint8_t { Initial, Resume };

class FunctionEmitter final {
 public:
  FunctionEmitter(llvm::Module& module,
                  const ir::Program& program,
                  std::size_t function_index,
                  std::span<const GeneratedFunction> generated,
                  const NativeRootPlan& roots,
                  NativeEntryKind kind)
      : module_(module),
        program_(program),
        source_(program.functions.at(function_index)),
        function_index_(function_index),
        generated_(generated),
        body_(kind == NativeEntryKind::Initial ? generated.subspan(function_index).front().body
                                               : generated.subspan(function_index).front().resume),
        kind_(kind),
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
  void Finish(llvm::Value* status);
  void EmitInteger(const ir::Instruction& instruction);
  void EmitDouble(const ir::Instruction& instruction);
  void EmitNumericConversion(const ir::Instruction& instruction, Builtin builtin);
  void EmitComparison(const ir::Instruction& instruction);
  void EmitValueEquality(const ir::Instruction& instruction);
  llvm::Value* ByteAddress(llvm::Value* object, std::uint64_t offset);
  llvm::Value* ReadHeapValue(llvm::Value* address, const SourceType& type);
  void WriteHeapValue(llvm::Value* address, ir::RegisterId reg);
  void EmitWriteBarrier(llvm::Value* object, ir::RegisterId reg);
  void EmitField(const ir::Instruction& instruction);
  void EmitGlobalLoad(const ir::Instruction& instruction);
  void EmitGlobalStore(const ir::Instruction& instruction);
  const layout::Type& ArrayElement(const ir::Instruction& instruction) const;
  llvm::Type* ElementStorage(const layout::Type& element);
  void WriteElement(llvm::Value* address, const layout::Type& element, ir::RegisterId reg);
  void EmitIndex(const ir::Instruction& instruction);
  void EmitArrayBuiltin(const ir::Instruction& instruction, Builtin builtin);
  std::expected<void, std::string> EmitReference(const ir::Instruction& instruction);
  llvm::Value* Encode(ir::RegisterId reg);
  llvm::Value* Pack(llvm::Value* encoded, std::size_t index);
  void Unpack(ir::RegisterId reg);
  void StoreRoots(std::size_t index);
  void ClearRoots(std::size_t index);
  llvm::Value* RootAddress(std::size_t slot);
  void ClearRoot(std::size_t slot);
  std::expected<void, std::string> EmitInstruction(const ir::Instruction& instruction);
  std::expected<void, std::string> PrepareFrame();
  [[nodiscard]] bool CanUseLeafEntry() const;
  void BindFrameStorage();
  void EmitFrameEntry();
  void CacheImports();
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
  const ir::Function& source_;
  std::size_t function_index_;
  std::size_t instruction_index_ = 0;
  std::span<const GeneratedFunction> generated_;
  llvm::Function* body_;
  NativeEntryKind kind_;
  Builder builder_;
  std::vector<llvm::Value*> slots_;
  std::vector<llvm::BasicBlock*> blocks_;
  llvm::BasicBlock* cleanup_ = nullptr;
  llvm::PHINode* status_ = nullptr;
  llvm::Value* arguments_ = nullptr;
  llvm::Value* result_ = nullptr;
  llvm::Value* instruction_slot_ = nullptr;
  llvm::Value* root_slots_ = nullptr;
  llvm::Value* frame_storage_ = nullptr;
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
    std::size_t function_index,
    std::span<const GeneratedFunction> generated);

}  // namespace vela::compiler::aot
