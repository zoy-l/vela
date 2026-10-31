#include "compiler/native/native_frame_plan.h"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Module.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>
#include "base/async_operation.h"
#include "base/builtin.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/function_emitter.h"
#include "compiler/native/native_roots.h"
#include "compiler/native/native_values.h"
#include "runtime/native/native_deoptimization.h"
#include "runtime/native/native_abi.h"
#include "runtime/value/value.h"
namespace vela::compiler::native {
namespace {
std::size_t TemporaryArgumentCount(const ir::Instruction& instruction) {
  switch (instruction.opcode) {
    case Opcode::CallIndirect:
      return static_cast<std::size_t>(instruction.index) + 2;
    case Opcode::CallInterface:
      return static_cast<std::size_t>(instruction.immediate);
    case Opcode::BuiltinCall:
      return BuiltinArity(static_cast<Builtin>(instruction.index));
    case Opcode::AsyncCall:
      return AsyncArity(static_cast<AsyncOperation>(instruction.index));
    case Opcode::StringInterpolate:
      return instruction.rhs;
    case Opcode::StringConcat:
    case Opcode::EqualString:
    case Opcode::NotEqualString:
    case Opcode::EqualValue:
    case Opcode::NotEqualValue:
      return 2;
    case Opcode::SetField:
    case Opcode::SetIndex:
    case Opcode::Print:
    case Opcode::SetGlobal:
    case Opcode::NewFuture:
    case Opcode::IsType:
    case Opcode::CheckedCast:
    case Opcode::ExceptionValue:
    case Opcode::Throw:
    case Opcode::Rethrow:
      return 1;
    default:
      return 0;
  }
}

}  // namespace
std::expected<std::size_t, std::string> PlanTemporaryCount(const ir::Function& source,
                                                           const ir::Program& program,
                                                           const NativeRootPlan& roots) {
  std::size_t scratch_count = roots.count == 0 && !source.is_async ? 0 : 1;
  scratch_count = std::max(scratch_count, roots.minimum_temporary_count);
  for (const auto& saved : roots.suspended) {
    if (!saved.empty()) {
      scratch_count = std::max(scratch_count, saved.size() + 1);
    }
  }
  for (const auto& instruction : source.instructions) {
    if ((instruction.opcode == Opcode::GetIndex || instruction.opcode == Opcode::SetIndex ||
         (instruction.opcode == Opcode::BuiltinCall &&
          instruction.index == static_cast<std::uint32_t>(Builtin::ArrayPush))) &&
        instruction.array_type == ir::kInvalidArrayType) {
      return std::unexpected("native code requires upstream array storage types");
    }
    scratch_count = std::max(scratch_count, TemporaryArgumentCount(instruction));
    if (program.native_mode == NativeProgramMode::Reloadable &&
        instruction.opcode == Opcode::Call) {
      scratch_count = std::max(
          scratch_count,
          static_cast<std::size_t>(program.functions.at(instruction.index).parameter_count));
    }
    const auto access = ir::AnalyzeRegisters(instruction, program.functions);
    bool has_type =
        !access.defines_value || source.value_types.at(instruction.dst).kind != Kind::Invalid;
    ir::ForEachUse(instruction, access, [&](ir::RegisterId reg) {
      has_type = has_type && source.value_types.at(reg).kind != Kind::Invalid;
    });
    if (!has_type) {
      return std::unexpected(
          "native function " + source.name + " is missing an upstream value type for opcode " +
          std::to_string(static_cast<unsigned>(instruction.opcode)) + " (index " +
          std::to_string(instruction.index) + ", result " + std::to_string(instruction.dst) + ")");
    }
  }
  return scratch_count;
}

std::expected<NativeFrameLayout, std::string> PlanNativeFrame(llvm::Module& module,
                                                              const ir::Program& program,
                                                              const EscapePlan& escape,
                                                              const NativeRootPlan& roots,
                                                              NativeCodeTier tier,
                                                              NativeEntryKind kind) {
  auto& context = module.getContext();
  const auto scratch_count = PlanTemporaryCount(escape.function, program, roots);
  if (!scratch_count) {
    return std::unexpected(scratch_count.error());
  }
  std::vector<llvm::Type*> fields;
  constexpr std::size_t kAuxiliaryFields = 6;
  fields.reserve(escape.function.register_count + kAuxiliaryFields);
  for (const auto& type : escape.function.value_types) {
    fields.push_back(type.kind == Kind::Invalid ? llvm::Type::getInt8Ty(context)
                                                : ValueType(context, type));
  }
  fields.push_back(llvm::ArrayType::get(NativeRecordType(context), *scratch_count));
  fields.push_back(NativeRecordType(context));
  auto* debug_storage = llvm::ArrayType::get(
      llvm::Type::getInt8Ty(context), escape.function.register_count * sizeof(runtime::NativeDebugValue));
  std::vector<llvm::Type*> recovery_fields{
      fields.at(escape.function.register_count),
      fields.at(escape.function.register_count + 1),
  };
  if (roots.debug_values) {
    recovery_fields.push_back(debug_storage);
  }
  if (tier == NativeCodeTier::Optimized) {
    auto* recovery_storage = llvm::StructType::get(context, recovery_fields);
    fields.push_back(
        llvm::ArrayType::get(llvm::Type::getInt8Ty(context), sizeof(runtime::NativeCheckpoint)));
    fields.push_back(
        llvm::ArrayType::get(NativeRecordType(context), escape.function.register_count));
    fields.push_back(recovery_storage);
    fields.push_back(llvm::ArrayType::get(llvm::PointerType::getUnqual(context),
                                          escape.checkpoints.empty() ? 0 : escape.reference_count));
  }
  if (kind == NativeEntryKind::RecoverInitial || kind == NativeEntryKind::RecoverResume) {
    auto* type = llvm::StructType::get(context, recovery_fields);
    return NativeFrameLayout{
        .type = type,
        .bytes =
            module.getDataLayout().getTypeAllocSize(type) + (roots.count * sizeof(runtime::Value)),
    };
  }
  for (const auto& site : escape.allocations) {
    if (site.stack_record) {
      fields.push_back(
          llvm::ArrayType::get(llvm::PointerType::getUnqual(context), site.fields.size()));
    }
  }
  if (roots.debug_values) {
    fields.push_back(debug_storage);
  }
  auto* type = llvm::StructType::get(context, fields);
  return NativeFrameLayout{
      .type = type,
      .bytes =
          module.getDataLayout().getTypeAllocSize(type) + (roots.count * sizeof(runtime::Value)),
  };
}

}  // namespace vela::compiler::native
