#include "compiler/native/function_emitter.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>
#include <bit>
#include <cstdint>
#include <limits>
#include "base/builtin.h"
#include "base/execution_status.h"
#include "base/opcode.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/ir.h"
#include "compiler/native/native_values.h"
#include "runtime/library/standard_library.h"
#include "runtime/native/native_function.h"

namespace vela::compiler::native {
namespace {
constexpr unsigned kIntegerShiftMask = 63;
constexpr double kIntegerMagnitudeLimit = 0x1p63;
}  // namespace

void FunctionEmitter::EmitIntrinsicImport() {
  using Intrinsic = runtime::NativeFunction::Intrinsic;
  const auto standard = runtime::FindStandardNative(source_.native_symbol);
  if (standard == nullptr || standard->aot_entry().intrinsic != Intrinsic::SquareRoot ||
      !standard->IsSignatureCompatible(*source_.signature, program_.layouts.types)) {
    return;
  }
  auto* entries = body_->getArg(static_cast<unsigned>(source_.parameter_count + 1));
  auto* intrinsic = builder_.CreateExtractValue(entries, 2);
  auto* operand = body_->getArg(1);
  auto* recognized = builder_.CreateICmpEQ(
      intrinsic, builder_.getInt8(static_cast<std::uint8_t>(Intrinsic::SquareRoot)));
  // 身份来自已冻结的真实绑定，宿主覆盖同名函数仍调用其入口。负参数沿用标准绑定的
  // NativeError 消息和逻辑栈；NaN 与 -0 按 sqrt 的浮点契约直接计算。
  auto* valid = builder_.CreateFCmpUGE(operand, llvm::ConstantFP::get(operand->getType(), 0));
  auto* calculate = llvm::BasicBlock::Create(module_.getContext(), "intrinsic.sqrt", body_);
  auto* host = llvm::BasicBlock::Create(module_.getContext(), "intrinsic.host", body_);
  builder_.CreateCondBr(builder_.CreateAnd(recognized, valid), calculate, host);
  builder_.SetInsertPoint(calculate);
  builder_.CreateStore(builder_.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, operand), Output());
  Finish(Status(builder_, ExecutionStatus::Returned));
  builder_.SetInsertPoint(host);
}

void FunctionEmitter::EmitInteger(const ir::Instruction& instruction) {
  auto* left = Payload(instruction.lhs);
  llvm::Value* result = nullptr;
  const auto opcode = instruction.opcode;
  if (opcode == Opcode::NegInt) {
    Check(builder_.CreateICmpNE(left, builder_.getInt64(std::bit_cast<std::uint64_t>(
                                          std::numeric_limits<std::int64_t>::min()))),
          ExecutionStatus::IntegerOverflow);
    result = builder_.CreateNeg(left);
  } else if (opcode == Opcode::BitNotInt) {
    result = builder_.CreateNot(left);
  } else {
    auto* right = Payload(instruction.rhs);
    switch (opcode) {
      case Opcode::AddInt:
      case Opcode::SubInt:
      case Opcode::MulInt: {
        auto intrinsic = llvm::Intrinsic::sadd_with_overflow;
        if (opcode == Opcode::SubInt) {
          intrinsic = llvm::Intrinsic::ssub_with_overflow;
        } else if (opcode == Opcode::MulInt) {
          intrinsic = llvm::Intrinsic::smul_with_overflow;
        }
        auto* checked = builder_.CreateBinaryIntrinsic(intrinsic, left, right);
        Check(builder_.CreateNot(builder_.CreateExtractValue(checked, 1)),
              ExecutionStatus::IntegerOverflow);
        result = builder_.CreateExtractValue(checked, 0);
        break;
      }
      case Opcode::DivInt:
      case Opcode::ModInt: {
        Check(builder_.CreateICmpNE(right, builder_.getInt64(0)), ExecutionStatus::DivisionByZero);
        auto* minimum = builder_.CreateICmpEQ(left, builder_.getInt64(std::bit_cast<std::uint64_t>(
                                                        std::numeric_limits<std::int64_t>::min())));
        auto* negative_one = builder_.CreateICmpEQ(
            right, builder_.getInt64(std::numeric_limits<std::uint64_t>::max()));
        Check(builder_.CreateNot(builder_.CreateAnd(minimum, negative_one)),
              ExecutionStatus::IntegerOverflow);
        result = opcode == Opcode::DivInt ? builder_.CreateSDiv(left, right)
                                          : builder_.CreateSRem(left, right);
        break;
      }
      case Opcode::BitAndInt:
        result = builder_.CreateAnd(left, right);
        break;
      case Opcode::BitOrInt:
        result = builder_.CreateOr(left, right);
        break;
      case Opcode::BitXorInt:
        result = builder_.CreateXor(left, right);
        break;
      case Opcode::ShiftLeftInt:
        result = builder_.CreateShl(left, builder_.CreateAnd(right, kIntegerShiftMask));
        break;
      case Opcode::ShiftRightInt:
        result = builder_.CreateAShr(left, builder_.CreateAnd(right, kIntegerShiftMask));
        break;
      case Opcode::ShiftUnsignedRightInt:
        result = builder_.CreateLShr(left, builder_.CreateAnd(right, kIntegerShiftMask));
        break;
      default:
        llvm_unreachable("integer opcode dispatch must be exhaustive");
    }
  }
  Store(instruction.dst, result, {.kind = Kind::Int, .name = frontend::kIntTypeName});
}

void FunctionEmitter::EmitDouble(const ir::Instruction& instruction) {
  auto* left = Payload(instruction.lhs);
  llvm::Value* result = nullptr;
  if (instruction.opcode == Opcode::NegDouble) {
    result = builder_.CreateFNeg(left);
  } else {
    auto* right = Payload(instruction.rhs);
    switch (instruction.opcode) {
      case Opcode::AddDouble:
        result = builder_.CreateFAdd(left, right);
        break;
      case Opcode::SubDouble:
        result = builder_.CreateFSub(left, right);
        break;
      case Opcode::MulDouble:
        result = builder_.CreateFMul(left, right);
        break;
      case Opcode::DivDouble:
        Check(builder_.CreateFCmpUNE(right, llvm::ConstantFP::get(right->getType(), 0)),
              ExecutionStatus::DivisionByZero);
        result = builder_.CreateFDiv(left, right);
        break;
      default:
        llvm_unreachable("double opcode dispatch must be exhaustive");
    }
  }
  Store(instruction.dst, result, {.kind = Kind::Double, .name = frontend::kDoubleTypeName});
}

void FunctionEmitter::EmitNumericConversion(const ir::Instruction& instruction, Builtin builtin) {
  const auto kind = source_.value_types.at(instruction.lhs).kind;
  auto* value = Payload(instruction.lhs);
  if (builtin == Builtin::NumericToDouble) {
    auto* converted =
        kind == Kind::Int ? builder_.CreateSIToFP(value, builder_.getDoubleTy()) : value;
    Store(instruction.dst, converted, {.kind = Kind::Double, .name = frontend::kDoubleTypeName});
    return;
  }
  if (kind == Kind::Double) {
    value = builder_.CreateUnaryIntrinsic(llvm::Intrinsic::trunc, value);
    // 与 ConvertNumber 共用半开范围语义；ordered 比较同时拒绝 NaN 和无穷。
    // 必须先检查再生成 fptosi，越界转换会产生 poison，不能用转换结果反查范围。
    auto* lower = llvm::ConstantFP::get(builder_.getDoubleTy(), -kIntegerMagnitudeLimit);
    auto* upper = llvm::ConstantFP::get(builder_.getDoubleTy(), kIntegerMagnitudeLimit);
    Check(builder_.CreateAnd(builder_.CreateFCmpOGE(value, lower),
                             builder_.CreateFCmpOLT(value, upper)),
          ExecutionStatus::InvalidNumericConversion);
    value = builder_.CreateFPToSI(value, builder_.getInt64Ty());
  }
  Store(instruction.dst, value, {.kind = Kind::Int, .name = frontend::kIntTypeName});
}

void FunctionEmitter::EmitComparison(const ir::Instruction& instruction) {
  auto* left = Payload(instruction.lhs);
  auto* right = Payload(instruction.rhs);
  llvm::Value* result = nullptr;
  switch (instruction.opcode) {
    case Opcode::EqualInt:
    case Opcode::EqualBool:
      result = builder_.CreateICmpEQ(left, right);
      break;
    case Opcode::NotEqualInt:
    case Opcode::NotEqualBool:
      result = builder_.CreateICmpNE(left, right);
      break;
    case Opcode::LessInt:
      result = builder_.CreateICmpSLT(left, right);
      break;
    case Opcode::LessEqualInt:
      result = builder_.CreateICmpSLE(left, right);
      break;
    case Opcode::GreaterInt:
      result = builder_.CreateICmpSGT(left, right);
      break;
    case Opcode::GreaterEqualInt:
      result = builder_.CreateICmpSGE(left, right);
      break;
    case Opcode::EqualDouble:
      result = builder_.CreateFCmpOEQ(left, right);
      break;
    case Opcode::NotEqualDouble:
      result = builder_.CreateFCmpUNE(left, right);
      break;
    case Opcode::LessDouble:
      result = builder_.CreateFCmpOLT(left, right);
      break;
    case Opcode::LessEqualDouble:
      result = builder_.CreateFCmpOLE(left, right);
      break;
    case Opcode::GreaterDouble:
      result = builder_.CreateFCmpOGT(left, right);
      break;
    case Opcode::GreaterEqualDouble:
      result = builder_.CreateFCmpOGE(left, right);
      break;
    default:
      llvm_unreachable("comparison opcode dispatch must be exhaustive");
  }
  Store(instruction.dst, result, {.kind = Kind::Bool, .name = frontend::kBoolTypeName});
}

void FunctionEmitter::EmitValueEquality(const ir::Instruction& instruction) {
  const auto& left_type = source_.value_types.at(instruction.lhs);
  const auto& right_type = source_.value_types.at(instruction.rhs);
  if (left_type.kind == Kind::Null || right_type.kind == Kind::Null) {
    const auto reg = left_type.kind == Kind::Null ? instruction.rhs : instruction.lhs;
    auto* present = IsPresent(builder_, Load(reg), source_.value_types.at(reg));
    auto* result = instruction.opcode == Opcode::EqualValue ? builder_.CreateNot(present) : present;
    Store(instruction.dst, result, {.kind = Kind::Bool, .name = frontend::kBoolTypeName});
    return;
  }
  if (!IsScalar(left_type) || !IsScalar(right_type)) {
    auto equal = module_.getOrInsertFunction("vela_native_v1_equal", builder_.getInt8Ty(),
                                             builder_.getPtrTy(), builder_.getPtrTy());
    auto* left = Pack(Encode(instruction.lhs), 0);
    auto* right = Pack(Encode(instruction.rhs), 1);
    auto* result =
        builder_.CreateICmpNE(builder_.CreateCall(equal, {left, right}), builder_.getInt8(0));
    if (instruction.opcode == Opcode::NotEqualValue) {
      result = builder_.CreateNot(result);
    }
    Store(instruction.dst, result, {.kind = Kind::Bool, .name = frontend::kBoolTypeName});
    return;
  }
  auto* left = Load(instruction.lhs);
  auto* right = Load(instruction.rhs);
  auto* left_present = left_type.nullable ? builder_.CreateExtractValue(left, 0)
                                          : builder_.getInt1(left_type.kind != Kind::Null);
  auto* right_present = right_type.nullable ? builder_.CreateExtractValue(right, 0)
                                            : builder_.getInt1(right_type.kind != Kind::Null);
  llvm::Value* payload_equal = builder_.getFalse();
  if (left_type.kind == right_type.kind && left_type.kind != Kind::Null) {
    auto* left_payload = left_type.nullable ? builder_.CreateExtractValue(left, 1) : left;
    auto* right_payload = right_type.nullable ? builder_.CreateExtractValue(right, 1) : right;
    payload_equal = left_type.kind == Kind::Double
                        ? builder_.CreateFCmpOEQ(left_payload, right_payload)
                        : builder_.CreateICmpEQ(left_payload, right_payload);
  }
  auto* both_null = builder_.CreateNot(builder_.CreateOr(left_present, right_present));
  auto* both_present = builder_.CreateAnd(left_present, right_present);
  auto* equal = builder_.CreateOr(both_null, builder_.CreateAnd(both_present, payload_equal));
  auto* result = instruction.opcode == Opcode::EqualValue ? equal : builder_.CreateNot(equal);
  Store(instruction.dst, result, {.kind = Kind::Bool, .name = frontend::kBoolTypeName});
}

}  // namespace vela::compiler::native
