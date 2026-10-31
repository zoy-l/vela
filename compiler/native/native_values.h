#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/IRBuilder.h>
#include <cstdint>
#include <string>
#include "base/execution_status.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/ir/ir.h"

namespace llvm {
class Type;
class LLVMContext;
class StructType;
class Value;
class ConstantInt;
class Module;
class Function;
}  // namespace llvm

namespace vela::compiler::native {

struct GeneratedFunction;

using SourceType = frontend::TypeDescriptor;
using Kind = SourceType::Kind;
using Builder = llvm::IRBuilder<>;

inline constexpr std::uint64_t kMaximumNativeStackBytes = 4096;

bool IsScalar(const SourceType& type);
bool IsReferenceKind(Kind kind);
bool HasValidity(const SourceType& type);
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
                        std::uint32_t function);
llvm::Value* IsPresent(Builder& builder, llvm::Value* value, const SourceType& type);
llvm::Value* EncodeValue(Builder& builder, llvm::Value* value, const SourceType& type);
llvm::Value* DecodeValue(Builder& builder, llvm::Value* record, const SourceType& type);
llvm::ConstantInt* Status(Builder& builder, ExecutionStatus status);
// 失败块直接返回原状态；成功块成为后续结果写入的插入点。
void ReturnOnFailure(Builder& builder, llvm::Function& function, llvm::Value* status);
std::uint8_t NativeKind(Kind kind);
llvm::Value* Convert(Builder& builder,
                     llvm::Value* value,
                     const SourceType& source,
                     const SourceType& target);
void EmitWrapper(llvm::Module& module,
                 const ir::Function& source,
                 std::uint32_t function_index,
                 const GeneratedFunction& function,
                 llvm::Constant* image);

}  // namespace vela::compiler::native
