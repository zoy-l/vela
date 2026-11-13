#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "base/execution_status.h"
#include "base/layout_description.h"
#include "base/native_program_mode.h"
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
class BasicBlock;
}  // namespace llvm

namespace vela::compiler::native {

struct GeneratedFunction;

using SourceType = frontend::TypeDescriptor;
using Kind = SourceType::Kind;
using Builder = llvm::IRBuilder<>;

inline constexpr std::uint64_t kMaximumNativeStackBytes = 4096;

// 叶入口的参数顺序为 execution、源参数、depth、可选递归 budget、result。
struct LeafEntryLayout final {
  explicit constexpr LeafEntryLayout(std::size_t parameter_count)
      : depth(static_cast<unsigned>(parameter_count + 1)), budget(depth + 1) {}

  unsigned depth;
  unsigned budget;
};

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
llvm::Value* LoadProgramImage(Builder& builder, llvm::Value* execution);
struct NativeObjectFields final {
  llvm::Value* payloads;
  llvm::Value* kinds;
};
// 只接受普通脚本实例；不接受宿主对象或 native callable 环境，地址不跨安全点借用。
NativeObjectFields LoadObjectFields(Builder& builder, llvm::Value* object, NativeProgramMode mode);
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
enum class NativeEntryContract : std::uint8_t { External, Reflection };
struct ReflectionCallInputs final {
  llvm::Value* execution;
  llvm::Value* receiver;
  llvm::Value* items;
  llvm::Value* snapshots;
};

// 在任何用户副作用前完成全部实参快照、类型和位置参数检查；不匹配交还完整 binder。
std::vector<llvm::Value*> UnpackReflectionArguments(Builder& builder,
                                                    const ir::Program& program,
                                                    const ir::Function& source,
                                                    ReflectionCallInputs inputs,
                                                    llvm::BasicBlock& unmatched);
void EmitWrapper(llvm::Module& module,
                 const ir::Program& program,
                 std::uint32_t function_index,
                 const GeneratedFunction& function,
                 llvm::Constant* image,
                 NativeCodePlacement placement,
                 NativeEntryContract contract);

}  // namespace vela::compiler::native
