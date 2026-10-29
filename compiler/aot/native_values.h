#pragma once

#include <llvm/IR/IRBuilder.h>
#include <cstdint>
#include "base/execution_status.h"
#include "compiler/frontend/type_checker.h"
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

namespace vela::compiler::aot {

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
llvm::Value* LoadImport(Builder& builder,
                        llvm::Module& module,
                        llvm::Value* execution,
                        std::uint32_t function);
llvm::Value* IsPresent(Builder& builder, llvm::Value* value, const SourceType& type);
llvm::Value* EncodeValue(Builder& builder, llvm::Value* value, const SourceType& type);
llvm::Value* DecodeValue(Builder& builder, llvm::Value* record, const SourceType& type);
llvm::ConstantInt* Status(Builder& builder, ExecutionStatus status);
std::uint8_t NativeKind(Kind kind);
llvm::Value* Convert(Builder& builder,
                     llvm::Value* value,
                     const SourceType& source,
                     const SourceType& target);
void EmitWrapper(llvm::Module& module,
                 const ir::Function& source,
                 std::uint32_t function_index,
                 const GeneratedFunction& function);

}  // namespace vela::compiler::aot
