#include "base/layout_description.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/native/function_emitter.h"
#include "runtime/native/native_symbols.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>
#include "base/builtin.h"
#include "base/execution_status.h"
#include "base/opcode.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/ir.h"
#include "compiler/native/native_values.h"
#include "compiler/native/struct_values.h"
#include "runtime/library/standard_library.h"
#include "runtime/native/native_function.h"

namespace vela::compiler::native {
namespace {
constexpr unsigned kIntegerShiftMask = 63;
constexpr double kIntegerMagnitudeLimit = 0x1p63;
constexpr auto kIntegerMinimumBits =
    std::bit_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::min());

}  // namespace

runtime::NativeFunction::Intrinsic FindMathIntrinsic(const ir::Function& function,
                                                     const layout::Program& layouts) {
  using Intrinsic = runtime::NativeFunction::Intrinsic;
  const auto standard = runtime::FindStandardNative(function.native_symbol);
  const auto& signature = function.signature;
  return standard != nullptr && signature && standard->IsSignatureCompatible(*signature, layouts)
             ? standard->aot_entry().intrinsic
             : Intrinsic::None;
}

llvm::Value* FunctionEmitter::IntrinsicCondition(llvm::Value& entries,
                                                 llvm::Value* operand,
                                                 runtime::NativeFunction::Intrinsic expected) {
  using Intrinsic = runtime::NativeFunction::Intrinsic;
  auto* intrinsic = builder_.CreateExtractValue(&entries, 2);
  auto* recognized =
      builder_.CreateICmpEQ(intrinsic, builder_.getInt8(static_cast<std::uint8_t>(expected)));
  // 身份来自已冻结的真实绑定，宿主覆盖同名函数仍调用其入口。负参数沿用标准绑定的
  // vela::Error 消息和逻辑栈；NaN 与 -0 按 sqrt 的浮点契约直接计算。
  if (expected == Intrinsic::SquareRoot) {
    auto* valid = builder_.CreateFCmpUGE(operand, llvm::ConstantFP::get(operand->getType(), 0));
    return builder_.CreateAnd(recognized, valid);
  }
  return recognized;
}

llvm::Value* FunctionEmitter::EmitMathIntrinsic(runtime::NativeFunction::Intrinsic intrinsic,
                                                std::span<llvm::Value* const> operands) {
  using Intrinsic = runtime::NativeFunction::Intrinsic;
  const char* symbol = nullptr;
  switch (intrinsic) {
    case Intrinsic::SquareRoot:
      return builder_.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, operands[0]);
    case Intrinsic::Atan2:
      return builder_.CreateBinaryIntrinsic(llvm::Intrinsic::atan2, operands[0], operands[1]);
    case Intrinsic::Round:
      return builder_.CreateUnaryIntrinsic(llvm::Intrinsic::round, operands[0]);
    case Intrinsic::CubeRoot:
      symbol = "cbrt";
      break;
    case Intrinsic::Hypotenuse:
      symbol = "hypot";
      break;
    case Intrinsic::Remainder:
      symbol = "remainder";
      break;
    case Intrinsic::None:
      llvm_unreachable("math lowering requires a recognized intrinsic");
  }
  std::vector<llvm::Type*> types(operands.size(), builder_.getDoubleTy());
  auto* signature = llvm::FunctionType::get(builder_.getDoubleTy(), types, false);
  return builder_.CreateCall(module_.getOrInsertFunction(symbol, signature),
                             llvm::ArrayRef(operands.data(), operands.size()));
}

void FunctionEmitter::EmitIntrinsicImport() {
  const auto intrinsic = generated_[function_index_].math_intrinsic;
  if (intrinsic == runtime::NativeFunction::Intrinsic::None) {
    return;
  }
  auto* entries = body_->getArg(static_cast<unsigned>(source_.parameter_count + 1));
  std::vector<llvm::Value*> operands;
  operands.reserve(source_.parameter_count);
  for (std::size_t index = 0; index < source_.parameter_count; ++index) {
    operands.push_back(body_->getArg(static_cast<unsigned>(index + 1)));
  }
  auto* calculate = llvm::BasicBlock::Create(module_.getContext(), "intrinsic.math", body_);
  auto* host = llvm::BasicBlock::Create(module_.getContext(), "intrinsic.host", body_);
  builder_.CreateCondBr(IntrinsicCondition(*entries, operands.front(), intrinsic), calculate, host);
  builder_.SetInsertPoint(calculate);
  builder_.CreateStore(EmitMathIntrinsic(intrinsic, operands), Output());
  Finish(Status(builder_, ExecutionStatus::Returned));
  builder_.SetInsertPoint(host);
}

void FunctionEmitter::EmitInteger(const ir::Instruction& instruction) {
  auto* left = Payload(instruction.lhs);
  llvm::Value* result = nullptr;
  const auto opcode = instruction.opcode;
  if (opcode == Opcode::NegInt) {
    if (!HasRangeProof(array_access_.no_overflow)) {
      Check(builder_.CreateICmpNE(left, builder_.getInt64(kIntegerMinimumBits)),
            ExecutionStatus::IntegerOverflow);
    }
    result = builder_.CreateNSWNeg(left);
  } else if (opcode == Opcode::BitNotInt) {
    result = builder_.CreateNot(left);
  } else {
    auto* right = Payload(instruction.rhs);
    switch (opcode) {
      case Opcode::AddInt:
      case Opcode::SubInt:
      case Opcode::MulInt: {
        if (HasRangeProof(array_access_.no_overflow)) {
          if (opcode == Opcode::AddInt) {
            result = builder_.CreateNSWAdd(left, right);
          } else if (opcode == Opcode::SubInt) {
            result = builder_.CreateNSWSub(left, right);
          } else {
            result = builder_.CreateNSWMul(left, right);
          }
          break;
        }
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
        auto* minimum = builder_.CreateICmpEQ(left, builder_.getInt64(kIntegerMinimumBits));
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
  if (const auto predicate = IntegerComparison(instruction.opcode)) {
    result = builder_.CreateICmp(*predicate, left, right);
    Store(instruction.dst, result, {.kind = Kind::Bool, .name = frontend::kBoolTypeName});
    return;
  }
  switch (instruction.opcode) {
    case Opcode::EqualBool:
      result = builder_.CreateICmpEQ(left, right);
      break;
    case Opcode::NotEqualBool:
      result = builder_.CreateICmpNE(left, right);
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

llvm::Value* FunctionEmitter::CompareInlineValues(llvm::Value* left,
                                                  llvm::Value* right,
                                                  const SourceType& type) {
  if (type.nullable && type.kind != Kind::Dynamic) {
    auto* left_present = IsPresent(builder_, left, type);
    auto* right_present = IsPresent(builder_, right, type);
    auto* both_present = builder_.CreateAnd(left_present, right_present);
    auto* same_presence = builder_.CreateICmpEQ(left_present, right_present);
    auto* absent = builder_.GetInsertBlock();
    auto* compare = llvm::BasicBlock::Create(module_.getContext(), "value.compare", body_);
    auto* done = llvm::BasicBlock::Create(module_.getContext(), "value.compared", body_);
    builder_.CreateCondBr(both_present, compare, done);
    builder_.SetInsertPoint(compare);
    auto required = type;
    required.nullable = false;
    auto* equal = CompareInlineValues(PayloadOf(builder_, left, type),
                                      PayloadOf(builder_, right, type), required);
    auto* compared = builder_.GetInsertBlock();
    builder_.CreateBr(done);
    builder_.SetInsertPoint(done);
    auto* result = builder_.CreatePHI(builder_.getInt1Ty(), 2);
    result->addIncoming(same_presence, absent);
    result->addIncoming(equal, compared);
    return result;
  }
  if (type.value_layout != nullptr) {
    llvm::Value* equal = builder_.getTrue();
    const auto& fields = type.value_layout->fields;
    for (unsigned field = 0; field < fields.size(); ++field) {
      auto* same =
          CompareInlineValues(ExtractStructField(builder_, left, type, field),
                              ExtractStructField(builder_, right, type, field), fields[field]);
      equal = builder_.CreateAnd(equal, same);
    }
    return equal;
  }
  if (IsScalar(type)) {
    if (type.kind == Kind::Null || type.kind == Kind::Void) {
      return builder_.getTrue();
    }
    return type.kind == Kind::Double ? builder_.CreateFCmpOEQ(left, right)
                                     : builder_.CreateICmpEQ(left, right);
  }
  auto* first = Pack(EncodeValue(builder_, left, type), 0);
  auto* second = Pack(EncodeValue(builder_, right, type), 1);
  return ComparePackedValues(first, second);
}

llvm::Value* FunctionEmitter::ComparePackedValues(llvm::Value* left, llvm::Value* right) {
  auto equal = module_.getOrInsertFunction(runtime::kNativeEqualSymbol, builder_.getInt8Ty(),
                                           builder_.getPtrTy(), builder_.getPtrTy());
  return builder_.CreateICmpNE(builder_.CreateCall(equal, {left, right}), builder_.getInt8(0));
}

void FunctionEmitter::EmitValueEquality(const ir::Instruction& instruction) {
  const auto& left_type = source_.value_types.at(instruction.lhs);
  const auto& right_type = source_.value_types.at(instruction.rhs);
  if (left_type.value_layout != nullptr && right_type.value_layout != nullptr &&
      frontend::NonNullableTypeName(left_type) == frontend::NonNullableTypeName(right_type)) {
    auto common = left_type;
    common.nullable = left_type.nullable || right_type.nullable;
    auto* left = ConvertValue(Load(instruction.lhs), left_type, common);
    auto* right = ConvertValue(Load(instruction.rhs), right_type, common);
    auto* equal = CompareInlineValues(left, right, common);
    auto* result = instruction.opcode == Opcode::EqualValue ? equal : builder_.CreateNot(equal);
    Store(instruction.dst, result, {.kind = Kind::Bool, .name = frontend::kBoolTypeName});
    return;
  }
  if (left_type.kind == Kind::Null || right_type.kind == Kind::Null) {
    const auto reg = left_type.kind == Kind::Null ? instruction.rhs : instruction.lhs;
    auto* present = IsPresent(builder_, Load(reg), source_.value_types.at(reg));
    auto* result = instruction.opcode == Opcode::EqualValue ? builder_.CreateNot(present) : present;
    Store(instruction.dst, result, {.kind = Kind::Bool, .name = frontend::kBoolTypeName});
    return;
  }
  if (left_type.kind == right_type.kind &&
      (left_type.kind == Kind::Array || left_type.kind == Kind::Map ||
       left_type.kind == Kind::Set)) {
    // 集合的身份不依赖内容或容量；可空引用的 null 已由同一个原生指针表示。
    auto* equal = builder_.CreateICmpEQ(Load(instruction.lhs), Load(instruction.rhs));
    auto* result = instruction.opcode == Opcode::EqualValue ? equal : builder_.CreateNot(equal);
    Store(instruction.dst, result, {.kind = Kind::Bool, .name = frontend::kBoolTypeName});
    return;
  }
  if (!IsScalar(left_type) || !IsScalar(right_type)) {
    auto* left = Pack(Encode(instruction.lhs), 0);
    auto* right = Pack(Encode(instruction.rhs), 1);
    auto* result = ComparePackedValues(left, right);
    if (instruction.opcode == Opcode::NotEqualValue) {
      result = builder_.CreateNot(result);
    }
    Store(instruction.dst, result, {.kind = Kind::Bool, .name = frontend::kBoolTypeName});
    return;
  }
  auto* left = Load(instruction.lhs);
  auto* right = Load(instruction.rhs);
  auto* left_present = IsPresent(builder_, left, left_type);
  auto* right_present = IsPresent(builder_, right, right_type);
  llvm::Value* payload_equal = builder_.getFalse();
  if (left_type.kind == right_type.kind) {
    auto* left_payload = PayloadOf(builder_, left, left_type);
    auto* right_payload = PayloadOf(builder_, right, right_type);
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
