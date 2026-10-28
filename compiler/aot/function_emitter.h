#pragma once

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>
#include "base/execution_status.h"
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
  void Finish(llvm::Value* status);
  void EmitInteger(const ir::Instruction& instruction);
  void EmitDouble(const ir::Instruction& instruction);
  void EmitComparison(const ir::Instruction& instruction);
  void EmitValueEquality(const ir::Instruction& instruction);
  std::expected<void, std::string> EmitReference(const ir::Instruction& instruction);
  llvm::Value* Encode(ir::RegisterId reg);
  llvm::Value* Pack(llvm::Value* encoded, std::size_t index);
  void Unpack(ir::RegisterId reg);
  void StoreRoots(std::size_t index);
  void ClearRoots(std::size_t index);
  std::expected<void, std::string> EmitInstruction(const ir::Instruction& instruction);
  std::expected<void, std::string> PrepareFrame();
  void BindFrameStorage();
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
  llvm::StructType* frame_type_ = nullptr;
  llvm::Value* frame_storage_ = nullptr;
  std::uint64_t frame_bytes_ = 0;
  const NativeRootPlan& roots_;
};

std::expected<NativeFunctionMetrics, std::string> EmitFunction(
    llvm::Module& module,
    const ir::Program& program,
    std::size_t function_index,
    std::span<const GeneratedFunction> generated);

}  // namespace vela::compiler::aot
