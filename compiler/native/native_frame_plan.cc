#include "compiler/native/native_frame_plan.h"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Module.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/function_emitter.h"
#include "compiler/native/native_roots.h"
#include "compiler/native/native_values.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_deoptimization.h"
#include "runtime/value/value.h"
namespace vela::compiler::native {
namespace {
std::size_t TemporaryArgumentCount(const ir::Instruction& instruction) {
  switch (instruction.opcode) {
    case Opcode::CallIndirect:
      return static_cast<std::size_t>(ir::CallArgumentCount(instruction)) + 2;
    case Opcode::CallInterface:
    case Opcode::BuiltinCall:
    case Opcode::AsyncCall:
    case Opcode::StringInterpolate:
      return ir::CallArgumentCount(instruction);
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
std::size_t PlanTemporaryCount(const ir::Function& source,
                               const ir::Program& program,
                               const NativeRootPlan& roots) {
  std::size_t scratch_count = roots.count == 0 && !source.is_async ? 0 : 1;
  scratch_count = std::max(scratch_count, roots.minimum_temporary_count);
  if (!source.native_symbol.empty()) {
    for (std::size_t index = 0; index < source.parameter_count; ++index) {
      const auto& type = source.value_types.at(index);
      if (type.kind == Kind::Dynamic || HasValidity(type)) {
        scratch_count = std::max(scratch_count, index + 1);
      }
    }
  }
  for (const auto& saved : roots.suspended) {
    if (!saved.empty()) {
      scratch_count = std::max(scratch_count, saved.size() + 1);
    }
  }
  for (const auto& instruction : source.instructions) {
    scratch_count = std::max(scratch_count, TemporaryArgumentCount(instruction));
    if (program.native_mode == NativeProgramMode::Reloadable &&
        instruction.opcode == Opcode::Call) {
      scratch_count = std::max(
          scratch_count,
          static_cast<std::size_t>(program.functions.at(instruction.index).parameter_count));
    }
  }
  return scratch_count;
}

NativeFrameLayout PlanNativeFrame(llvm::Module& module,
                                  const EscapePlan& escape,
                                  const NativeRootPlan& roots,
                                  std::size_t temporary_count,
                                  NativeFrameRequirements requirements,
                                  NativeEntryKind kind,
                                  std::size_t auxiliary_bytes) {
  const auto& function = escape.function.get();
  const auto has_checkpoints = !escape.checkpoints.empty();
  const auto reference_count = has_checkpoints ? escape.reference_count : 0;
  const auto virtual_state_bytes = has_checkpoints ? sizeof(runtime::NativeVirtualState) : 0;
  auto& context = module.getContext();
  std::vector<llvm::Type*> fields;
  constexpr std::size_t kAuxiliaryFields = 7;
  fields.reserve(function.register_count + kAuxiliaryFields);
  std::vector<std::uint8_t> addressed(function.register_count);
  for (const auto& allocation : escape.allocations) {
    if (allocation.stack_record) {
      for (const auto reg : allocation.fields) {
        addressed.at(reg) = 1;
      }
    }
    if (has_checkpoints && allocation.can_scalar_replace) {
      addressed.at(allocation.reference) = 1;
    }
  }
  std::vector<std::optional<unsigned>> local_fields(addressed.size());
  for (std::size_t index = 0; index < addressed.size(); ++index) {
    if (addressed.at(index) != 0) {
      local_fields.at(index) = static_cast<unsigned>(fields.size());
      fields.push_back(ValueType(context, function.value_types.at(index)));
    }
  }
  // 仅借用记录和物化需要稳定局部地址。其余局部值由独立 alloca 提升为 SSA，
  // 不应因检查点或暂存参数较大而全部退化为堆中的寄存器文件。
  const auto temporary_field = static_cast<unsigned>(fields.size());
  fields.push_back(llvm::ArrayType::get(NativeRecordType(context), temporary_count));
  fields.push_back(NativeRecordType(context));
  auto* debug_storage =
      roots.debug_values
          ? llvm::ArrayType::get(llvm::Type::getInt8Ty(context),
                                 function.register_count * sizeof(runtime::NativeDebugValue))
          : nullptr;
  std::vector<llvm::Type*> recovery_fields{
      fields.at(temporary_field),
      fields.at(temporary_field + 1),
  };
  if (roots.debug_values) {
    recovery_fields.push_back(debug_storage);
  }
  if (requirements.recovery) {
    auto* recovery_storage = llvm::StructType::get(context, recovery_fields);
    fields.push_back(
        llvm::ArrayType::get(llvm::Type::getInt8Ty(context), sizeof(runtime::NativeCheckpoint)));
    fields.push_back(llvm::ArrayType::get(NativeRecordType(context), function.register_count));
    fields.push_back(recovery_storage);
    fields.push_back(llvm::ArrayType::get(llvm::PointerType::getUnqual(context), reference_count));
    fields.push_back(llvm::ArrayType::get(llvm::Type::getInt8Ty(context), virtual_state_bytes));
  }
  if (kind == NativeEntryKind::RecoverInitial || kind == NativeEntryKind::RecoverResume) {
    auto* type = llvm::StructType::get(context, recovery_fields);
    return NativeFrameLayout{
        .type = type,
        .bytes =
            module.getDataLayout().getTypeAllocSize(type) + (roots.count * sizeof(runtime::Value)),
        .local_fields = {},
        .leaf_result_field = std::nullopt,
        .auxiliary_field = std::nullopt,
    };
  }
  for (const auto& site : escape.allocations) {
    if (site.stack_record) {
      fields.push_back(
          llvm::ArrayType::get(llvm::PointerType::getUnqual(context), site.fields.size()));
    }
  }
  std::optional<unsigned> leaf_result_field;
  if (requirements.leaf_calls) {
    leaf_result_field = static_cast<unsigned>(fields.size());
    fields.push_back(NativeRecordType(context));
  }
  std::optional<unsigned> auxiliary_field;
  if (auxiliary_bytes != 0) {
    auxiliary_field = static_cast<unsigned>(fields.size());
    auto* unit = llvm::FixedVectorType::get(llvm::Type::getInt64Ty(context), 2);
    const auto alignment = runtime::kNativeFrameAlignment;
    fields.push_back(llvm::ArrayType::get(unit, (auxiliary_bytes + alignment - 1) / alignment));
  }
  if (roots.debug_values) {
    fields.push_back(debug_storage);
  }
  auto* type = llvm::StructType::get(context, fields);
  return NativeFrameLayout{
      .type = type,
      .bytes =
          module.getDataLayout().getTypeAllocSize(type) + (roots.count * sizeof(runtime::Value)),
      .local_fields = std::move(local_fields),
      .temporary_field = temporary_field,
      .leaf_result_field = leaf_result_field,
      .auxiliary_field = auxiliary_field,
  };
}

}  // namespace vela::compiler::native
