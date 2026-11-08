#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "base/execution_status.h"
#include "base/layout_description.h"
#include "base/opcode.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/ir.h"
#include "compiler/native/code_generator.h"

namespace llvm {
class Type;
class LLVMContext;
class StructType;
class Value;
class ConstantInt;
class Module;
class Function;
class LoadInst;
}  // namespace llvm

namespace vela::compiler::native {

struct GeneratedFunction;

using SourceType = frontend::TypeDescriptor;
using Kind = SourceType::Kind;
using Builder = llvm::IRBuilder<>;

inline constexpr std::uint64_t kMaximumNativeStackBytes = 4096;

// 只追踪单定义和 Move 链；返回的字符串常量由实例保根，不依赖函数栈根槽。
std::vector<std::optional<std::uint32_t>> FindStringConstants(
    const ir::Function& function,
    std::span<const ir::Function> functions);

bool IsScalar(const SourceType& type);
bool IsReferenceKind(Kind kind);
bool HasValidity(const SourceType& type);
bool IsSquareRootImport(const ir::Function& function, const layout::Program& layouts);
std::optional<llvm::CmpInst::Predicate> IntegerComparison(Opcode opcode);
void AssumeNonNull(Builder& builder, llvm::Value* value, const SourceType& type);
llvm::Type* PayloadType(llvm::LLVMContext& context, Kind kind);
llvm::Type* ValueType(llvm::LLVMContext& context, const SourceType& type);
llvm::StructType* NativeRecordType(llvm::LLVMContext& context);
llvm::StructType* NativeImportType(llvm::LLVMContext& context);
llvm::Constant* EmitConstantTable(llvm::Module& module,
                                  llvm::StructType* table,
                                  llvm::Type* element,
                                  llvm::ArrayRef<llvm::Constant*> values,
                                  const std::string& name);
llvm::Value* LoadImport(Builder& builder,
                        llvm::Module& module,
                        llvm::Value* execution,
                        std::uint32_t function,
                        NativeCodePlacement placement);
llvm::Value* LoadProgramData(Builder& builder, llvm::Value* execution);
llvm::Value* IsPresent(Builder& builder, llvm::Value* value, const SourceType& type);
llvm::Value* PayloadOf(Builder& builder, llvm::Value* value, const SourceType& type);
llvm::Value* MakeOptional(Builder& builder,
                          llvm::Value* present,
                          llvm::Value* payload,
                          const SourceType& type);
void MarkInvariant(llvm::LoadInst& load);
llvm::Value* EncodeValue(Builder& builder, llvm::Value* value, const SourceType& type);
llvm::Value* DecodeValue(Builder& builder, llvm::Value* record, const SourceType& type);
llvm::ConstantInt* Status(Builder& builder, ExecutionStatus status);
// 失败块直接返回原状态；成功块成为后续结果写入的插入点。
void ReturnOnFailure(Builder& builder, llvm::Function& function, llvm::Value* status);
// 装箱入口和异步恢复边界验证 execution，并清理最外层调用遗留的异常。
void EmitExecutionBoundary(Builder& builder);
std::uint8_t NativeKind(Kind kind);
llvm::Value* Convert(Builder& builder,
                     llvm::Value* value,
                     const SourceType& source,
                     const SourceType& target);
void EmitWrapper(llvm::Module& module,
                 const ir::Function& source,
                 std::uint32_t function_index,
                 const GeneratedFunction& function,
                 llvm::Constant* image,
                 NativeCodePlacement placement);

}  // namespace vela::compiler::native
