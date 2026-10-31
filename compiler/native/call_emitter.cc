#include <llvm/IR/DerivedTypes.h>
#include <cstddef>
#include <optional>
#include <vector>
#include "base/declaration.h"
#include "base/native_program_mode.h"
#include "compiler/ir/ir.h"
#include "compiler/native/function_emitter.h"
#include "compiler/native/native_values.h"
#include "runtime/native/native_memory_layout.h"
namespace vela::compiler::native {
void FunctionEmitter::EmitCall(const ir::Instruction& instruction) {
  const auto& callee = program_.functions.at(instruction.index);
  const auto borrowed = escape_.borrowed_calls.empty()
                            ? std::optional<std::size_t>{}
                            : escape_.borrowed_calls.at(instruction_index_);
  if (program_.native_mode == NativeProgramMode::Reloadable && callee.native_symbol.empty() &&
      tier_ == NativeCodeTier::Baseline && callee.identity.local == 0 &&
      callee.identity.declaration != DeclarationId{}) {
    for (std::size_t index = 0; index < callee.parameter_count; ++index) {
      const auto reg = static_cast<ir::RegisterId>(instruction.lhs + index);
      const auto& type = callee.value_types.at(index);
      auto* value = Convert(builder_, Load(reg), source_.value_types.at(reg), type);
      Pack(EncodeValue(builder_, value, type), index);
    }
    auto helper = module_.getOrInsertFunction(
        "vela_native_v3_call_declaration", builder_.getInt8Ty(), builder_.getPtrTy(),
        builder_.getInt64Ty(), builder_.getPtrTy(), builder_.getInt64Ty(), builder_.getPtrTy());
    CompleteCall(builder_.CreateCall(helper,
                                     {
                                         body_->getArg(0),
                                         builder_.getInt64(callee.identity.declaration.value),
                                         arguments_,
                                         builder_.getInt64(callee.parameter_count),
                                         result_,
                                     }),
                 instruction.dst);
    return;
  }
  std::vector<llvm::Value*> arguments{body_->getArg(0)};
  for (std::size_t index = 0; index < callee.parameter_count; ++index) {
    if (index == 0 && borrowed) {
      arguments.push_back(records_.at(*borrowed));
      continue;
    }
    const auto reg = static_cast<ir::RegisterId>(instruction.lhs + index);
    arguments.push_back(
        Convert(builder_, Load(reg), source_.value_types.at(reg), callee.value_types.at(index)));
  }
  if (!callee.native_symbol.empty()) {
    arguments.push_back(imports_.at(instruction.index));
  }
  // 各类结果都可放入对齐的单个 ABI 暂存槽；循环和多调用不增加帧大小。
  arguments.push_back(result_);
  // Call 的效果要求调用者先登记逻辑帧；内联后仍保留该事实，避免为嵌套宿主调用
  // 复制一条不可能发生的最外层入口清理路径。
  auto* depth = builder_.CreateLoad(
      builder_.getInt64Ty(),
      ByteAddress(body_->getArg(0), runtime::NativeMemoryLayout::kExecutionDepth));
  builder_.CreateAssumption(builder_.CreateICmpNE(depth, builder_.getInt64(0)));
  const auto& target = generated_.subspan(instruction.index).front();
  auto* status = builder_.CreateCall(borrowed ? target.borrowed : target.body, arguments);
  if (borrowed) {
    // 被调用方可能在抛错前修改引用字段；错误封套分配前刷新调用者根。
    StoreRoots(instruction_index_);
  }
  CheckStatus(status);
  if (callee.return_type.kind == Kind::Void) {
    Store(instruction.dst, builder_.getFalse(), callee.return_type);
  } else {
    Store(instruction.dst,
          builder_.CreateLoad(ValueType(module_.getContext(), callee.return_type), result_),
          callee.return_type);
  }
}

}  // namespace vela::compiler::native
