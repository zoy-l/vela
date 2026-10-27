#include "compiler/aot/native_compiler.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

#include "base/execution_status.h"
#include "base/layout_description.h"
#include "base/opcode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"
#include "runtime/native_abi.h"

namespace vela::compiler::aot {
namespace {

using runtime::NativeValueKind;

constexpr std::size_t kNativeValueBytes = sizeof(runtime::NativeValue);
constexpr std::uint8_t kReturned = static_cast<std::uint8_t>(ExecutionStatus::Returned);

struct FunctionTypes final {
  std::vector<layout::Type::Kind> parameters;
  std::optional<layout::Type::Kind> result;
};

struct Generated final {
  llvm::Function* body = nullptr;
  llvm::Function* entry = nullptr;
  FunctionTypes types;
};

struct ModuleBuilder final {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  llvm::Type* byte = nullptr;
  llvm::Type* byte_pointer = nullptr;
  llvm::Type* word = nullptr;
  llvm::Type* native_value_pointer = nullptr;
  llvm::FunctionCallee frame_enter;
  llvm::FunctionCallee frame_exit;
  llvm::FunctionCallee copy_argument;
  llvm::FunctionCallee copy;
  llvm::FunctionCallee store_null;
  llvm::FunctionCallee store_bool;
  llvm::FunctionCallee store_int;
  llvm::FunctionCallee store_double;
  llvm::FunctionCallee store_result;
  llvm::FunctionCallee int_binary;
  llvm::FunctionCallee int_unary;
  llvm::FunctionCallee double_binary;
  llvm::FunctionCallee double_unary;
  llvm::FunctionCallee bool_binary;
  llvm::FunctionCallee compare;
  llvm::FunctionCallee is_false;
  llvm::FunctionCallee is_null;

  explicit ModuleBuilder(std::string name) : module(std::make_unique<llvm::Module>(name, context)) {
    byte = llvm::Type::getInt8Ty(context);
    byte_pointer = llvm::PointerType::getUnqual(context);
    word = llvm::Type::getInt64Ty(context);
    native_value_pointer = byte_pointer;
    const auto status = byte;
    const auto kind = byte;
    const auto boolean = llvm::Type::getInt1Ty(context);
    const auto void_type = llvm::Type::getVoidTy(context);
    const auto context_pointer = byte_pointer;
    frame_enter = module->getOrInsertFunction(
        "vela_native_frame_enter", llvm::FunctionType::get(status, {context_pointer, word}, false));
    frame_exit = module->getOrInsertFunction(
        "vela_native_frame_exit", llvm::FunctionType::get(void_type, {context_pointer}, false));
    copy_argument = module->getOrInsertFunction(
        "vela_native_copy_argument",
        llvm::FunctionType::get(
            status, {native_value_pointer, native_value_pointer, word, word, kind}, false));
    copy = module->getOrInsertFunction(
        "vela_native_copy",
        llvm::FunctionType::get(status, {native_value_pointer, native_value_pointer}, false));
    store_null = module->getOrInsertFunction(
        "vela_native_store_null", llvm::FunctionType::get(status, {native_value_pointer}, false));
    store_bool = module->getOrInsertFunction(
        "vela_native_store_bool",
        llvm::FunctionType::get(status, {native_value_pointer, boolean}, false));
    store_int = module->getOrInsertFunction(
        "vela_native_store_int",
        llvm::FunctionType::get(status, {native_value_pointer, word}, false));
    store_double = module->getOrInsertFunction(
        "vela_native_store_double",
        llvm::FunctionType::get(status, {native_value_pointer, llvm::Type::getDoubleTy(context)},
                                false));
    store_result = module->getOrInsertFunction(
        "vela_native_store_result",
        llvm::FunctionType::get(status, {native_value_pointer, native_value_pointer, kind}, false));
    const auto operation = byte;
    int_binary = module->getOrInsertFunction(
        "vela_native_int_binary",
        llvm::FunctionType::get(
            status, {native_value_pointer, native_value_pointer, native_value_pointer, operation},
            false));
    int_unary = module->getOrInsertFunction(
        "vela_native_int_unary",
        llvm::FunctionType::get(status, {native_value_pointer, native_value_pointer, operation},
                                false));
    double_binary = module->getOrInsertFunction(
        "vela_native_double_binary",
        llvm::FunctionType::get(
            status, {native_value_pointer, native_value_pointer, native_value_pointer, operation},
            false));
    double_unary = module->getOrInsertFunction(
        "vela_native_double_unary",
        llvm::FunctionType::get(status, {native_value_pointer, native_value_pointer, operation},
                                false));
    bool_binary = module->getOrInsertFunction(
        "vela_native_bool_binary",
        llvm::FunctionType::get(
            status, {native_value_pointer, native_value_pointer, native_value_pointer, operation},
            false));
    compare = module->getOrInsertFunction(
        "vela_native_compare",
        llvm::FunctionType::get(
            status, {native_value_pointer, native_value_pointer, native_value_pointer, operation},
            false));
    is_false = module->getOrInsertFunction(
        "vela_native_is_false",
        llvm::FunctionType::get(status, {native_value_pointer, byte_pointer}, false));
    is_null = module->getOrInsertFunction(
        "vela_native_is_null",
        llvm::FunctionType::get(status, {native_value_pointer, byte_pointer}, false));
  }
};

frontend::Diagnostic Diagnostic(const ir::Function& function,
                                std::span<const std::string> paths,
                                std::string message) {
  return {.location = function.location,
          .message = std::move(message),
          .source_path = paths.empty() ? std::string() : paths[function.location.source_id]};
}

std::optional<NativeValueKind> Kind(const layout::Type& type) {
  switch (type.kind) {
    case layout::Type::Kind::Boolean:
      return NativeValueKind::Boolean;
    case layout::Type::Kind::Integer:
    case layout::Type::Kind::Enum:
      return NativeValueKind::Integer;
    case layout::Type::Kind::Double:
      return NativeValueKind::Double;
    case layout::Type::Kind::Dynamic:
      return std::nullopt;
    default:
      return std::nullopt;
  }
}

std::expected<FunctionTypes, frontend::Diagnostic> Types(const ir::Program& program,
                                                         const ir::Function& function) {
  if (function.is_async || function.signature == std::nullopt) {
    return std::unexpected(Diagnostic(function, program.source_paths,
                                      "AOT synchronous backend requires a typed sync function"));
  }
  if (function.signature->hidden_parameters != 0 ||
      function.parameter_count != function.signature->parameters.size()) {
    return std::unexpected(
        Diagnostic(function, program.source_paths,
                   "AOT synchronous backend requires a plain function signature"));
  }
  FunctionTypes result;
  result.parameters.reserve(function.signature->parameters.size());
  for (const auto& parameter : function.signature->parameters) {
    const auto& type = program.layouts.types.at(parameter.type);
    if (type.nullable || !Kind(type)) {
      return std::unexpected(
          Diagnostic(function, program.source_paths,
                     "AOT synchronous backend supports only non-null scalar parameters"));
    }
    result.parameters.push_back(type.kind);
  }
  if (function.signature->result) {
    const auto& type = program.layouts.types.at(*function.signature->result);
    if (type.nullable || !Kind(type)) {
      return std::unexpected(
          Diagnostic(function, program.source_paths,
                     "AOT synchronous backend supports only non-null scalar results"));
    }
    result.result = type.kind;
  }
  return result;
}

std::uint8_t KindValue(layout::Type::Kind kind) {
  switch (kind) {
    case layout::Type::Kind::Boolean:
      return static_cast<std::uint8_t>(NativeValueKind::Boolean);
    case layout::Type::Kind::Integer:
    case layout::Type::Kind::Enum:
      return static_cast<std::uint8_t>(NativeValueKind::Integer);
    case layout::Type::Kind::Double:
      return static_cast<std::uint8_t>(NativeValueKind::Double);
    default:
      return static_cast<std::uint8_t>(NativeValueKind::Null);
  }
}

std::uint8_t CompareCode(Opcode opcode) {
  switch (opcode) {
    case Opcode::EqualInt:
    case Opcode::EqualDouble:
    case Opcode::EqualBool:
    case Opcode::EqualValue:
      return 0;
    case Opcode::NotEqualInt:
    case Opcode::NotEqualDouble:
    case Opcode::NotEqualBool:
    case Opcode::NotEqualValue:
      return 1;
    case Opcode::LessInt:
    case Opcode::LessDouble:
      return 2;
    case Opcode::LessEqualInt:
    case Opcode::LessEqualDouble:
      return 3;
    case Opcode::GreaterInt:
    case Opcode::GreaterDouble:
      return 4;
    case Opcode::GreaterEqualInt:
    case Opcode::GreaterEqualDouble:
      return 5;
    default:
      return 0;
  }
}

llvm::Value* Slot(llvm::IRBuilder<>& builder,
                  llvm::Value* registers,
                  std::size_t index,
                  llvm::Type* word) {
  return builder.CreateInBoundsGEP(word, registers, builder.getInt64(index));
}

llvm::Value* Status(llvm::IRBuilder<>& builder, std::uint8_t status) {
  return builder.getInt8(status);
}

std::expected<Generated, frontend::Diagnostic> GenerateFunction(
    ModuleBuilder& builder,
    const ir::Program& program,
    const ir::Function& function,
    std::size_t function_index,
    std::span<const Generated> generated) {
  auto types = Types(program, function);
  if (!types)
    return std::unexpected(types.error());
  const auto& declaration = generated[function_index];
  auto* body = declaration.body;
  auto* entry = declaration.entry;
  auto* entry_context = entry->arg_begin();
  auto* entry_arguments = std::next(entry->arg_begin());
  auto* entry_count = std::next(entry->arg_begin(), 2);
  auto* entry_result = std::next(entry->arg_begin(), 3);
  auto* entry_block = llvm::BasicBlock::Create(builder.context, "entry", entry);
  llvm::IRBuilder<> entry_builder(entry_block);
  auto* entered =
      entry_builder.CreateCall(builder.frame_enter, {entry_context, entry_builder.getInt64(0)});
  auto* enter_ok = entry_builder.CreateICmpEQ(entered, Status(entry_builder, kReturned));
  auto* invoke = llvm::BasicBlock::Create(builder.context, "invoke", entry);
  auto* enter_fail = llvm::BasicBlock::Create(builder.context, "enter_fail", entry);
  entry_builder.CreateCondBr(enter_ok, invoke, enter_fail);
  llvm::IRBuilder<> fail_builder(enter_fail);
  fail_builder.CreateRet(entered);
  llvm::IRBuilder<> invoke_builder(invoke);
  auto* body_result =
      invoke_builder.CreateCall(body, {entry_context, entry_arguments, entry_count, entry_result});
  invoke_builder.CreateCall(builder.frame_exit, {entry_context});
  invoke_builder.CreateRet(body_result);

  auto* body_entry = llvm::BasicBlock::Create(builder.context, "body_entry", body);
  llvm::IRBuilder<> ir(builder.context);
  ir.SetInsertPoint(body_entry);
  auto* registers =
      ir.CreateAlloca(builder.byte, ir.getInt64(function.register_count * kNativeValueBytes));
  auto* return_block = llvm::BasicBlock::Create(builder.context, "return", body);
  llvm::IRBuilder<> return_builder(return_block);
  return_builder.CreateRet(Status(return_builder, kReturned));
  auto* failure = llvm::BasicBlock::Create(builder.context, "failure", body);
  llvm::IRBuilder<> failure_builder(failure);
  // Each operation branches to the shared failure block after writing its status.
  auto* status_phi = failure_builder.CreatePHI(builder.byte, 8, "status");
  failure_builder.CreateRet(status_phi);
  for (std::size_t index = 0; index < function.parameter_count; ++index) {
    const auto expected = KindValue(types->parameters.at(index));
    auto* status = ir.CreateCall(
        builder.copy_argument,
        {Slot(ir, registers, index * kNativeValueBytes, builder.byte), std::next(body->arg_begin()),
         std::next(body->arg_begin(), 2), ir.getInt64(index), ir.getInt8(expected)});
    auto* okay = llvm::BasicBlock::Create(builder.context, "argument_ok", body);
    auto* failed = llvm::BasicBlock::Create(builder.context, "argument_fail", body);
    ir.CreateCondBr(ir.CreateICmpEQ(status, Status(ir, kReturned)), okay, failed);
    llvm::IRBuilder<> failed_builder(failed);
    failed_builder.CreateBr(failure);
    status_phi->addIncoming(status, failed);
    ir.SetInsertPoint(okay);
  }

  auto* context = body->arg_begin();
  std::vector<llvm::BasicBlock*> blocks(function.instructions.size());
  for (std::size_t index = 0; index < blocks.size(); ++index) {
    blocks[index] =
        llvm::BasicBlock::Create(builder.context, "block" + std::to_string(index), body);
  }
  const auto next_block = [&](std::size_t index) {
    return index + 1 < blocks.size() ? blocks[index + 1] : return_block;
  };
  const auto target_block = [&](std::uint32_t index) {
    return index < blocks.size() ? blocks[index] : return_block;
  };
  auto emit_status = [&](llvm::IRBuilder<>& current, llvm::Value* status, llvm::BasicBlock* next) {
    auto* okay = llvm::BasicBlock::Create(builder.context, "op_ok", body);
    current.CreateCondBr(current.CreateICmpEQ(status, Status(current, kReturned)), okay, failure);
    status_phi->addIncoming(status, current.GetInsertBlock());
    current.SetInsertPoint(okay);
    current.CreateBr(next);
    return okay;
  };
  // The initial argument chain enters the first IR block.
  ir.CreateBr(function.instructions.empty() ? return_block : blocks.front());

  for (std::size_t index = 0; index < function.instructions.size(); ++index) {
    ir.SetInsertPoint(blocks.at(index));
    const auto& instruction = function.instructions.at(index);
    const auto dst = Slot(ir, registers, instruction.dst * kNativeValueBytes, builder.byte);
    const auto lhs = Slot(ir, registers, instruction.lhs * kNativeValueBytes, builder.byte);
    const auto rhs = Slot(ir, registers, instruction.rhs * kNativeValueBytes, builder.byte);
    switch (instruction.opcode) {
      case Opcode::LoadNull:
        emit_status(ir, ir.CreateCall(builder.store_null, {dst}), next_block(index));
        break;
      case Opcode::LoadBool:
        emit_status(
            ir, ir.CreateCall(builder.store_bool, {dst, ir.getInt1(instruction.immediate != 0)}),
            next_block(index));
        break;
      case Opcode::LoadInt:
        emit_status(
            ir,
            ir.CreateCall(builder.store_int,
                          {dst, ir.getInt64(static_cast<std::uint64_t>(instruction.immediate))}),
            next_block(index));
        break;
      case Opcode::LoadIntConstant:
        return std::unexpected(
            Diagnostic(function, program.source_paths,
                       "integer constants must be lowered before AOT compilation"));
      case Opcode::LoadDouble:
        if (instruction.index >= function.doubles.size()) {
          return std::unexpected(
              Diagnostic(function, program.source_paths, "invalid double constant in AOT IR"));
        }
        emit_status(
            ir,
            ir.CreateCall(builder.store_double,
                          {dst, llvm::ConstantFP::get(llvm::Type::getDoubleTy(builder.context),
                                                      function.doubles.at(instruction.index))}),
            next_block(index));
        break;
      case Opcode::Move:
        emit_status(ir, ir.CreateCall(builder.copy, {dst, lhs}), next_block(index));
        break;
      case Opcode::AddInt:
      case Opcode::SubInt:
      case Opcode::MulInt:
      case Opcode::DivInt:
      case Opcode::ModInt:
      case Opcode::BitAndInt:
      case Opcode::BitOrInt:
      case Opcode::BitXorInt:
      case Opcode::ShiftLeftInt:
      case Opcode::ShiftRightInt:
      case Opcode::ShiftUnsignedRightInt:
        emit_status(ir,
                    ir.CreateCall(builder.int_binary,
                                  {dst, lhs, rhs,
                                   ir.getInt8(static_cast<std::uint8_t>(instruction.opcode) -
                                              static_cast<std::uint8_t>(Opcode::AddInt))}),
                    next_block(index));
        break;
      case Opcode::NegInt:
      case Opcode::BitNotInt:
        emit_status(
            ir,
            ir.CreateCall(builder.int_unary,
                          {dst, lhs, ir.getInt8(instruction.opcode == Opcode::NegInt ? 0 : 1)}),
            next_block(index));
        break;
      case Opcode::AddDouble:
      case Opcode::SubDouble:
      case Opcode::MulDouble:
      case Opcode::DivDouble:
        emit_status(ir,
                    ir.CreateCall(builder.double_binary,
                                  {dst, lhs, rhs,
                                   ir.getInt8(static_cast<std::uint8_t>(instruction.opcode) -
                                              static_cast<std::uint8_t>(Opcode::AddDouble))}),
                    next_block(index));
        break;
      case Opcode::NegDouble:
        emit_status(ir, ir.CreateCall(builder.double_unary, {dst, lhs, ir.getInt8(0)}),
                    next_block(index));
        break;
      case Opcode::AndBool:
      case Opcode::OrBool:
        emit_status(ir,
                    ir.CreateCall(
                        builder.bool_binary,
                        {dst, lhs, rhs, ir.getInt8(instruction.opcode == Opcode::AndBool ? 0 : 1)}),
                    next_block(index));
        break;
      case Opcode::EqualInt:
      case Opcode::NotEqualInt:
      case Opcode::LessInt:
      case Opcode::LessEqualInt:
      case Opcode::GreaterInt:
      case Opcode::GreaterEqualInt:
      case Opcode::EqualDouble:
      case Opcode::NotEqualDouble:
      case Opcode::LessDouble:
      case Opcode::LessEqualDouble:
      case Opcode::GreaterDouble:
      case Opcode::GreaterEqualDouble:
      case Opcode::EqualBool:
      case Opcode::NotEqualBool:
      case Opcode::EqualValue:
      case Opcode::NotEqualValue:
        emit_status(ir,
                    ir.CreateCall(builder.compare,
                                  {dst, lhs, rhs, ir.getInt8(CompareCode(instruction.opcode))}),
                    next_block(index));
        break;
      case Opcode::NotBool: {
        auto* temporary = ir.CreateAlloca(builder.byte);
        auto* status = ir.CreateCall(builder.is_false, {lhs, temporary});
        auto* next = llvm::BasicBlock::Create(builder.context, "not_ok", body);
        ir.CreateCondBr(ir.CreateICmpEQ(status, Status(ir, kReturned)), next, failure);
        status_phi->addIncoming(status, ir.GetInsertBlock());
        ir.SetInsertPoint(next);
        auto* store_status =
            ir.CreateCall(builder.store_bool, {dst, ir.CreateLoad(builder.byte, temporary)});
        emit_status(ir, store_status, next_block(index));
        break;
      }
      case Opcode::Jump:
        ir.CreateBr(target_block(instruction.index));
        break;
      case Opcode::JumpIfFalse:
      case Opcode::JumpIfNull: {
        auto* temporary = ir.CreateAlloca(builder.byte);
        auto* status = ir.CreateCall(
            instruction.opcode == Opcode::JumpIfFalse ? builder.is_false : builder.is_null,
            {lhs, temporary});
        auto* okay = llvm::BasicBlock::Create(builder.context, "branch_ok", body);
        ir.CreateCondBr(ir.CreateICmpEQ(status, Status(ir, kReturned)), okay, failure);
        status_phi->addIncoming(status, ir.GetInsertBlock());
        ir.SetInsertPoint(okay);
        auto* condition = ir.CreateLoad(builder.byte, temporary);
        ir.CreateCondBr(ir.CreateICmpNE(condition, ir.getInt8(0)), target_block(instruction.index),
                        next_block(index));
        break;
      }
      case Opcode::Call: {
        if (instruction.index >= generated.size()) {
          return std::unexpected(
              Diagnostic(function, program.source_paths, "invalid AOT call target"));
        }
        const auto& callee = generated[instruction.index];
        auto* called =
            ir.CreateCall(callee.entry->getFunctionType(), callee.entry,
                          {context, lhs, ir.getInt64(callee.types.parameters.size()), dst});
        emit_status(ir, called, next_block(index));
        break;
      }
      case Opcode::Return:
        if (types->result) {
          emit_status(ir,
                      ir.CreateCall(builder.store_result, {std::next(body->arg_begin(), 3), dst,
                                                           ir.getInt8(KindValue(*types->result))}),
                      return_block);
        } else {
          emit_status(ir, ir.CreateCall(builder.store_null, {std::next(body->arg_begin(), 3)}),
                      return_block);
        }
        break;
      default:
        return std::unexpected(Diagnostic(function, program.source_paths,
                                          "instruction is outside the synchronous native backend"));
    }
  }
  if (function.instructions.empty()) {
    ir.CreateBr(return_block);
  }
  for (auto* block : blocks) {
    if (block->getTerminator() == nullptr) {
      llvm::IRBuilder<> repair(block);
      repair.CreateBr(return_block);
    }
  }
  return Generated{.body = body, .entry = entry, .types = std::move(*types)};
}

std::expected<std::vector<std::byte>, std::string> EmitObject(llvm::Module& module,
                                                              std::string* target_name) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  const auto triple = llvm::sys::getDefaultTargetTriple();
  std::string error;
  const auto* target = llvm::TargetRegistry::lookupTarget(triple, error);
  if (target == nullptr)
    return std::unexpected(std::move(error));
  llvm::TargetOptions options;
  auto machine = std::unique_ptr<llvm::TargetMachine>(
      target->createTargetMachine(llvm::Triple(triple), "generic", "", options, llvm::Reloc::PIC_));
  if (machine == nullptr)
    return std::unexpected("unable to create native target machine");
  module.setTargetTriple(llvm::Triple(triple));
  module.setDataLayout(machine->createDataLayout());
  llvm::SmallVector<char, 0> bytes;
  llvm::raw_svector_ostream stream(bytes);
  llvm::legacy::PassManager pass;
  if (machine->addPassesToEmitFile(pass, stream, nullptr, llvm::CodeGenFileType::ObjectFile)) {
    return std::unexpected("native target cannot emit object files");
  }
  pass.run(module);
  if (target_name != nullptr)
    *target_name = triple;
  std::vector<std::byte> result(bytes.size());
  std::transform(bytes.begin(), bytes.end(), result.begin(), [](char value) {
    return static_cast<std::byte>(static_cast<unsigned char>(value));
  });
  return result;
}

}  // namespace

std::expected<NativeArtifact, frontend::Diagnostics> Compile(const ir::Program& program,
                                                             NativeOptions options) {
  ModuleBuilder builder(std::move(options.module_name));
  std::vector<Generated> generated;
  generated.reserve(program.functions.size());
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    auto types = Types(program, program.functions.at(index));
    if (!types)
      return std::unexpected(frontend::Diagnostics{std::move(types.error())});
    const auto wrapper_type = llvm::FunctionType::get(
        builder.byte,
        {builder.byte_pointer, builder.byte_pointer, builder.word, builder.byte_pointer}, false);
    auto* body =
        llvm::Function::Create(wrapper_type, llvm::GlobalValue::InternalLinkage,
                               "vela_aot_body_" + std::to_string(index), builder.module.get());
    auto* entry =
        llvm::Function::Create(wrapper_type, llvm::GlobalValue::ExternalLinkage,
                               "vela_aot_entry_" + std::to_string(index), builder.module.get());
    generated.push_back({.body = body, .entry = entry, .types = std::move(*types)});
  }
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    auto result = GenerateFunction(builder, program, program.functions.at(index), index, generated);
    if (!result)
      return std::unexpected(frontend::Diagnostics{std::move(result.error())});
    generated[index].types = std::move(result->types);
  }
  std::string verification;
  llvm::raw_string_ostream verifier_output(verification);
  if (llvm::verifyModule(*builder.module, &verifier_output)) {
    return std::unexpected(frontend::Diagnostics{
        {.location = {}, .message = verifier_output.str(), .source_path = {}}});
  }
  std::string llvm_ir;
  llvm::raw_string_ostream ir_output(llvm_ir);
  builder.module->print(ir_output, nullptr);
  std::string target;
  auto object = EmitObject(*builder.module, &target);
  if (!object) {
    return std::unexpected(
        frontend::Diagnostics{{.location = {}, .message = object.error(), .source_path = {}}});
  }
  return NativeArtifact{
      .object = std::move(*object), .llvm_ir = std::move(llvm_ir), .target = std::move(target)};
}

}  // namespace vela::compiler::aot
