#include "compiler/native/function_emitter.h"
#include "compiler/native/integer_loop_optimizer.h"
#include "runtime/native/native_symbols.h"

#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/ModRef.h>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>
#include "base/builtin.h"
#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/ir.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/map_analysis.h"
#include "compiler/native/native_values.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_memory_layout.h"
#include "runtime/strings/string.h"
#include "runtime/value/value.h"

namespace vela::compiler::native {
namespace {

void MarkHeapAllocation(llvm::Function& function) {
  function.addRetAttr(llvm::Attribute::NoAlias);
  function.addRetAttr(llvm::Attribute::getWithDereferenceableOrNullBytes(
      function.getContext(), sizeof(runtime::HeapObject)));
}

}  // namespace

std::size_t FunctionEmitter::PlanMapLookupStorage() {
  if (program_.native_mode != NativeProgramMode::Closed || !roots_.bounded_polls) {
    return 0;
  }
  map_lookup_pairs_ = FindMapLookups(source_, program_.functions);
  std::vector<bool> allocated(source_.instructions.size());
  std::size_t bytes = 0;
  const auto alignment = runtime::kNativeFrameAlignment;
  const auto lookup_bytes =
      ((runtime::NativeMemoryLayout::kMapLookupBytes + alignment - 1) / alignment) * alignment;
  for (std::size_t index = 0; index < map_lookup_pairs_.size(); ++index) {
    auto& get = map_lookup_pairs_[index];
    if (!get) {
      continue;
    }
    const auto& instruction = source_.instructions[*get];
    const auto& key = source_.value_types[instruction.lhs + 1];
    const auto& value = source_.value_types[instruction.dst];
    const auto& stored = source_.value_types[source_.instructions[index].lhs + 2];
    if (key.kind != Kind::String || key.nullable || value.kind != Kind::Int || !value.nullable ||
        stored.kind != Kind::Int || stored.nullable) {
      get.reset();
      continue;
    }
    if (!allocated[*get]) {
      allocated[*get] = true;
      bytes += lookup_bytes;
    }
  }
  return bytes;
}

void FunctionEmitter::CacheMapLookups() {
  if (placement_ != NativeCodePlacement::Linked ||
      program_.native_mode != NativeProgramMode::Closed || !roots_.bounded_polls) {
    return;
  }
  const auto& lookups = map_lookup_pairs_;
  map_lookups_.resize(source_.instructions.size());
  for (std::size_t index = 0; index < lookups.size(); ++index) {
    if (!lookups[index]) {
      continue;
    }
    const auto get = *lookups[index];
    auto*& lookup = map_lookups_[get];
    if (lookup == nullptr) {
      lookup = AllocateAuxiliary(runtime::NativeMemoryLayout::kMapLookupBytes);
    }
    map_lookups_[index] = lookup;
  }
}

void FunctionEmitter::CacheConstants() {
  llvm::Value* storage = nullptr;
  for (const auto& instruction : source_.instructions) {
    if (instruction.opcode != Opcode::LoadString || constants_.contains(instruction.index)) {
      continue;
    }
    if (storage == nullptr) {
      auto* state = LoadProgramData(builder_, body_->getArg(0));
      storage = builder_.CreateLoad(
          builder_.getPtrTy(), ByteAddress(state, runtime::NativeMemoryLayout::kProgramConstants));
      storage = builder_.CreateLoad(
          builder_.getPtrTy(),
          ByteAddress(storage, runtime::NativeMemoryLayout::kObjectNativePayload));
    }
    // 常量池属于当前执行借用的版本实例，发布不会改写旧池；重入返回后也恢复该实例。
    // 实例发布前已创建全部字符串常量；非移动 GC 和实例根保证整个调用期间可借用。
    const auto offset =
        (instruction.index * sizeof(runtime::Value)) + runtime::NativeMemoryLayout::kValuePayload;
    constants_.emplace(instruction.index,
                       builder_.CreateLoad(builder_.getPtrTy(), ByteAddress(storage, offset)));
  }
}

llvm::Value* FunctionEmitter::Encode(ir::RegisterId reg) {
  return EncodeValue(builder_, Load(reg), source_.value_types.at(reg));
}

llvm::Value* FunctionEmitter::Pack(llvm::Value* encoded, std::size_t index) {
  auto* slot =
      builder_.CreateConstGEP1_64(NativeRecordType(module_.getContext()), arguments_, index);
  builder_.CreateStore(encoded, slot);
  return slot;
}

void FunctionEmitter::Unpack(ir::RegisterId reg) {
  auto* record = builder_.CreateLoad(NativeRecordType(module_.getContext()), result_);
  const auto& type = source_.value_types.at(reg);
  auto* value = DecodeValue(builder_, record, type);
  AssumeNonNull(builder_, value, type);
  Store(reg, value, type);
}

void FunctionEmitter::PackArguments(ir::RegisterId first,
                                    std::size_t count,
                                    ArgumentPrefix prefix) {
  const auto offset = static_cast<std::size_t>(prefix);
  for (std::size_t argument = 0; argument < count; ++argument) {
    Pack(Encode(static_cast<ir::RegisterId>(first + argument)), argument + offset);
  }
}

void FunctionEmitter::CompleteCall(llvm::Value* status, ir::RegisterId result) {
  CheckStatus(status);
  Unpack(result);
}

void FunctionEmitter::StoreRoots(std::size_t index) {
  if (deferred_roots_) {
    return;
  }
  for (const auto reg : roots_.stores.at(index)) {
    const auto& slot = roots_.slots.at(reg);
    StoreRootValue(reg, RootAddress(*slot));
  }
}

void FunctionEmitter::StoreRootValue(ir::RegisterId reg, llvm::Value* address) {
  auto stored = LoadStoredValue(reg);
  WriteStoredValue(RecordAddress(address, MemoryRegion::Roots), stored);
}

void FunctionEmitter::ClearRoots(std::size_t index) {
  if (leaf_entry_ || deferred_roots_) {
    return;
  }
  for (const auto reg : roots_.clears.at(index)) {
    const auto& slot = roots_.slots.at(reg);
    ClearRoot(*slot);
  }
}

llvm::Value* FunctionEmitter::RootAddress(std::size_t slot) {
  return ByteAddress(root_slots_, slot * sizeof(runtime::Value));
}

void FunctionEmitter::ClearRoot(std::size_t slot) {
  auto* clear = StoreMemory(builder_.getInt8(NativeKind(Kind::Null)),
                            ByteAddress(RootAddress(slot), runtime::NativeMemoryLayout::kValueKind),
                            MemoryRegion::Roots);
  clear->setMetadata(kNativeRootClearMetadata, llvm::MDNode::get(module_.getContext(), {}));
}

void FunctionEmitter::EmitStringCodePointAt(const ir::Instruction& instruction) {
  using runtime::NativeMemoryLayout;
  auto* string = Reference(instruction.lhs);
  CheckReference(string);
  auto* index = Payload(instruction.lhs + 1);
  auto* length =
      LoadMemory(builder_.getInt32Ty(), ByteAddress(string, NativeMemoryLayout::kStringLength),
                 MemoryRegion::HeapHeader);
  MarkInvariant(*length);
  Check(builder_.CreateICmpULT(index, builder_.CreateZExt(length, builder_.getInt64Ty())),
        ExecutionStatus::IndexOutOfBounds);
  auto* bytes =
      LoadMemory(builder_.getInt32Ty(), ByteAddress(string, NativeMemoryLayout::kObjectLength),
                 MemoryRegion::HeapHeader);
  MarkInvariant(*bytes);
  auto& context = module_.getContext();
  auto* ascii = llvm::BasicBlock::Create(context, "string.index.ascii", body_);
  auto* unicode = llvm::BasicBlock::Create(context, "string.index.unicode", body_);
  auto* done = llvm::BasicBlock::Create(context, "string.index.done", body_);
  // 有效 UTF-8 的字节数等于码点数时，每个元素恰好是一个 ASCII 字节。
  builder_.CreateCondBr(builder_.CreateICmpEQ(bytes, length), ascii, unicode);
  builder_.SetInsertPoint(ascii);
  auto* data = ByteAddress(string, sizeof(runtime::StringObject));
  auto* point = builder_.CreateLoad(builder_.getInt8Ty(),
                                    builder_.CreateInBoundsGEP(builder_.getInt8Ty(), data, index));
  Store(instruction.dst, builder_.CreateZExt(point, builder_.getInt64Ty()),
        {.kind = Kind::Int, .name = frontend::kIntTypeName});
  builder_.CreateBr(done);
  builder_.SetInsertPoint(unicode);
  EmitRuntimeBuiltin(instruction);
  builder_.CreateBr(done);
  builder_.SetInsertPoint(done);
}

void FunctionEmitter::EmitLocatedMapBuiltin(const ir::Instruction& instruction,
                                            Builtin builtin,
                                            llvm::Value* map) {
  const auto& value_type =
      source_.value_types[builtin == Builtin::MapGet ? instruction.dst : instruction.lhs + 2];
  auto* pointer = builder_.getPtrTy();
  auto* key = Reference(instruction.lhs + 1);
  if (builtin == Builtin::MapGet) {
    auto get =
        module_.getOrInsertFunction(runtime::kNativeStringIntMapGetSymbol, builder_.getInt8Ty(),
                                    pointer, pointer, pointer, pointer);
    auto& function = *llvm::cast<llvm::Function>(get.getCallee());
    function.setMemoryEffects(llvm::MemoryEffects::readOnly().getWithModRef(
        llvm::MemoryEffects::Location::ArgMem, llvm::ModRefInfo::ModRef));
    function.addParamAttr(1, llvm::Attribute::ReadOnly);
    function.addParamAttr(2, llvm::Attribute::WriteOnly);
    function.addParamAttr(2, llvm::Attribute::NoAlias);
    function.addParamAttr(3, llvm::Attribute::WriteOnly);
    function.addParamAttr(3, llvm::Attribute::NoAlias);
    auto* present = builder_.CreateICmpNE(
        builder_.CreateCall(get, {map, key, result_, map_lookups_[instruction_index_]}),
        builder_.getInt8(0));
    auto* value = builder_.CreateLoad(builder_.getInt64Ty(), result_);
    Store(instruction.dst, MakeOptional(builder_, present, value, value_type), value_type);
  } else {
    auto* location = map_lookups_[instruction_index_];
    auto* value_slot = builder_.CreateLoad(
        pointer, ByteAddress(location, runtime::NativeMemoryLayout::kMapLookupIntegerValue));
    auto& context = module_.getContext();
    auto* update = llvm::BasicBlock::Create(context, "map.update", body_);
    auto* insert = llvm::BasicBlock::Create(context, "map.insert", body_);
    auto* done = llvm::BasicBlock::Create(context, "map.stored", body_);
    builder_.CreateCondBr(builder_.CreateIsNotNull(value_slot), update, insert);
    builder_.SetInsertPoint(update);
    builder_.CreateStore(Payload(instruction.lhs + 2), value_slot);
    builder_.CreateBr(done);
    builder_.SetInsertPoint(insert);
    PrepareInlineRuntimeCall();
    auto set =
        module_.getOrInsertFunction(runtime::kNativeStringIntMapSetSymbol, builder_.getInt8Ty(),
                                    pointer, pointer, pointer, builder_.getInt64Ty(), pointer);
    CheckStatus(builder_.CreateCall(set, {
                                             body_->getArg(0),
                                             map,
                                             key,
                                             Payload(instruction.lhs + 2),
                                             location,
                                         }));
    builder_.CreateBr(done);
    builder_.SetInsertPoint(done);
    Store(instruction.dst, builder_.getFalse(),
          {.kind = Kind::Null, .name = frontend::kNullTypeName});
  }
}

void FunctionEmitter::EmitMapBuiltin(const ir::Instruction& instruction, Builtin builtin) {
  auto* map = Reference(instruction.lhs);
  CheckReference(map);
  if (!map_lookups_.empty() && map_lookups_[instruction_index_] != nullptr) {
    EmitLocatedMapBuiltin(instruction, builtin, map);
    return;
  }
  auto* key = Pack(Encode(instruction.lhs + 1), 0);
  auto* pointer = builder_.getPtrTy();
  if (builtin == Builtin::MapGet) {
    auto get = module_.getOrInsertFunction(runtime::kNativeMapGetSymbol, builder_.getVoidTy(),
                                           pointer, pointer, pointer);
    auto& function = *llvm::cast<llvm::Function>(get.getCallee());
    function.setMemoryEffects(llvm::MemoryEffects::readOnly().getWithModRef(
        llvm::MemoryEffects::Location::ArgMem, llvm::ModRefInfo::ModRef));
    function.addParamAttr(1, llvm::Attribute::ReadOnly);
    function.addParamAttr(2, llvm::Attribute::WriteOnly);
    function.addParamAttr(2, llvm::Attribute::NoAlias);
    builder_.CreateCall(get, {map, key, result_});
    Unpack(instruction.dst);
  } else {
    auto set = module_.getOrInsertFunction(runtime::kNativeMapSetSymbol, builder_.getInt8Ty(),
                                           pointer, pointer, pointer, pointer);
    auto* value = Pack(Encode(instruction.lhs + 2), 1);
    CheckStatus(builder_.CreateCall(set, {body_->getArg(0), map, key, value}));
    Store(instruction.dst, builder_.getFalse(),
          {.kind = Kind::Null, .name = frontend::kNullTypeName});
  }
}

void FunctionEmitter::EmitAppendCodePoint(const ir::Instruction& instruction) {
  using runtime::NativeMemoryLayout;
  constexpr std::uint64_t kAsciiLimit = 0x80;
  auto* output = Reference(instruction.lhs);
  auto* point = Payload(instruction.lhs + 1);
  CheckReference(output);
  auto* length_address = ByteAddress(output, NativeMemoryLayout::kObjectLength);
  auto* length = LoadMemory(builder_.getInt32Ty(), length_address, MemoryRegion::HeapHeader);
  auto* capacity = LoadMemory(builder_.getInt64Ty(),
                              ByteAddress(output, NativeMemoryLayout::kMutableStringCapacity),
                              MemoryRegion::HeapHeader);
  auto* wide_length = builder_.CreateZExt(length, builder_.getInt64Ty());
  auto* available =
      builder_.CreateAnd(builder_.CreateICmpULT(point, builder_.getInt64(kAsciiLimit)),
                         builder_.CreateICmpULT(wide_length, capacity));
  auto* append = llvm::BasicBlock::Create(module_.getContext(), "string.append.ascii", body_);
  auto* runtime = llvm::BasicBlock::Create(module_.getContext(), "string.append.runtime", body_);
  auto* done = llvm::BasicBlock::Create(module_.getContext(), "string.append.done", body_);
  builder_.CreateCondBr(available, append, runtime);
  builder_.SetInsertPoint(append);
  // 容量充足的 ASCII 写入不会分配；UTF-8 编码、扩容和失败仍由原运行时操作负责。
  auto* data =
      LoadMemory(builder_.getPtrTy(), ByteAddress(output, NativeMemoryLayout::kMutableStringData),
                 MemoryRegion::HeapHeader);
  StoreMemory(builder_.CreateTrunc(point, builder_.getInt8Ty()),
              builder_.CreateGEP(builder_.getInt8Ty(), data, wide_length), MemoryRegion::Elements);
  StoreMemory(builder_.CreateNUWAdd(length, builder_.getInt32(1)), length_address,
              MemoryRegion::HeapHeader);
  builder_.CreateBr(done);
  builder_.SetInsertPoint(runtime);
  PrepareInlineRuntimeCall();
  CheckStatus(CallRuntimeBuiltin(instruction));
  PollAfterRuntimeCall();
  builder_.CreateBr(done);
  builder_.SetInsertPoint(done);
  Store(instruction.dst, builder_.getFalse(),
        {.kind = Kind::Null, .name = frontend::kNullTypeName});
}

std::expected<void, std::string> FunctionEmitter::EmitBuiltin(const ir::Instruction& instruction) {
  const auto builtin = static_cast<Builtin>(instruction.index);
  if (placement_ == NativeCodePlacement::Linked &&
      builtin == Builtin::MutableStringAppendCodePoint) {
    EmitAppendCodePoint(instruction);
    return {};
  }
  if (placement_ == NativeCodePlacement::Linked && builtin == Builtin::StringCodePointAt) {
    EmitStringCodePointAt(instruction);
    return {};
  }
  if (builtin == Builtin::ScalarToString) {
    const auto& type = source_.value_types.at(instruction.lhs);
    if (type.nullable ||
        (type.kind != Kind::Int && type.kind != Kind::Double && type.kind != Kind::Bool)) {
      return std::unexpected("native scalar formatting requires a non-null scalar operand");
    }
    auto format =
        module_.getOrInsertFunction(runtime::kNativeScalarToStringSymbol, builder_.getPtrTy(),
                                    builder_.getPtrTy(), builder_.getPtrTy());
    auto* string =
        builder_.CreateCall(format, {body_->getArg(0), Pack(Encode(instruction.lhs), 0)});
    Check(builder_.CreateIsNotNull(string), ExecutionStatus::OutOfMemory);
    Store(instruction.dst, string, {.kind = Kind::String, .name = frontend::kStringTypeName});
    return {};
  }
  if (builtin == Builtin::ScalarTextLength) {
    auto length = module_.getOrInsertFunction(runtime::kNativeScalarTextLengthSymbol,
                                              builder_.getInt64Ty(), builder_.getPtrTy());
    Store(instruction.dst, builder_.CreateCall(length, {Pack(Encode(instruction.lhs), 0)}),
          {.kind = Kind::Int, .name = frontend::kIntTypeName});
    return {};
  }
  if (builtin == Builtin::NumericToInt || builtin == Builtin::NumericToDouble) {
    const auto& type = source_.value_types.at(instruction.lhs);
    if (type.nullable || (type.kind != Kind::Int && type.kind != Kind::Double)) {
      return std::unexpected("native numeric conversion requires a non-null numeric operand");
    }
    EmitNumericConversion(instruction, builtin);
    return {};
  }
  if (builtin == Builtin::ArrayLength || builtin == Builtin::ArrayCapacity ||
      builtin == Builtin::ArrayMutation || builtin == Builtin::ArrayCheckMutation ||
      builtin == Builtin::ArrayPush) {
    EmitArrayBuiltin(instruction, builtin);
    return {};
  }
  if (builtin == Builtin::StringLength || builtin == Builtin::StringByteLength) {
    auto* string = Reference(instruction.lhs);
    CheckReference(string);
    const auto offset = builtin == Builtin::StringLength
                            ? runtime::NativeMemoryLayout::kStringLength
                            : runtime::NativeMemoryLayout::kObjectLength;
    auto* length = builder_.CreateLoad(builder_.getInt32Ty(), ByteAddress(string, offset));
    Store(instruction.dst, builder_.CreateZExt(length, builder_.getInt64Ty()),
          {.kind = Kind::Int, .name = frontend::kIntTypeName});
    return {};
  }
  if (builtin == Builtin::MapGet || builtin == Builtin::MapSet) {
    EmitMapBuiltin(instruction, builtin);
    return {};
  }
  if (builtin == Builtin::ArrayReserve) {
    using runtime::NativeMemoryLayout;
    auto* array = Reference(instruction.lhs);
    CheckReference(array);
    auto* length_address = ByteAddress(array, NativeMemoryLayout::kObjectLength);
    auto* length = LoadMemory(builder_.getInt32Ty(), length_address, MemoryRegion::HeapHeader);
    auto* requested = Payload(instruction.lhs + 1);
    EmitRuntimeBuiltin(instruction);
    // reserve 的成功契约保留长度并满足容量；将运行时保证交给 LLVM 消除后续扩容分支。
    auto* after = LoadMemory(builder_.getInt32Ty(), length_address, MemoryRegion::HeapHeader);
    auto* capacity =
        LoadMemory(builder_.getInt32Ty(), ByteAddress(array, NativeMemoryLayout::kArrayCapacity),
                   MemoryRegion::HeapHeader);
    builder_.CreateAssumption(builder_.CreateICmpEQ(after, length));
    builder_.CreateAssumption(
        builder_.CreateICmpUGE(builder_.CreateZExt(capacity, builder_.getInt64Ty()), requested));
    return {};
  }
  EmitRuntimeBuiltin(instruction);
  return {};
}

void FunctionEmitter::EmitRuntimeBuiltin(const ir::Instruction& instruction) {
  CompleteCall(CallRuntimeBuiltin(instruction), instruction.dst);
}

llvm::Value* FunctionEmitter::CallRuntimeBuiltin(const ir::Instruction& instruction) {
  PackArguments(instruction.lhs, BuiltinArity(static_cast<Builtin>(instruction.index)));
  auto helper = module_.getOrInsertFunction(runtime::kNativeBuiltinSymbol, builder_.getInt8Ty(),
                                            builder_.getPtrTy(), builder_.getInt32Ty(),
                                            builder_.getPtrTy(), builder_.getPtrTy());
  return builder_.CreateCall(
      helper, {body_->getArg(0), builder_.getInt32(instruction.index), arguments_, result_});
}

std::expected<void, std::string> FunctionEmitter::EmitReference(
    const ir::Instruction& instruction) {
  auto* context = body_->getArg(0);
  auto* pointer = builder_.getPtrTy();
  auto* word = builder_.getInt32Ty();
  auto* status_type = builder_.getInt8Ty();
  auto* const index = builder_.getInt32(instruction.index);
  llvm::Value* status = nullptr;
  switch (instruction.opcode) {
    case Opcode::LoadString: {
      Store(instruction.dst, constants_.at(instruction.index), {.kind = Kind::String, .name = {}});
      return {};
    }
    case Opcode::NewInstance: {
      if (!program_.classes.at(instruction.index).callable) {
        auto helper =
            module_.getOrInsertFunction(runtime::kNativeNewObjectSymbol, pointer, pointer, word);
        auto& allocation = *llvm::cast<llvm::Function>(helper.getCallee());
        // 普通实例一定是新分配的对象；可调用值的版本绑定和潜在身份复用单独处理。
        MarkHeapAllocation(allocation);
        auto* object = builder_.CreateCall(helper, {context, index});
        Check(builder_.CreateIsNotNull(object), ExecutionStatus::OutOfMemory);
        if (program_.native_mode == NativeProgramMode::Closed) {
          using runtime::NativeMemoryLayout;
          auto* count = LoadMemory(word, ByteAddress(object, NativeMemoryLayout::kObjectFieldCount),
                                   MemoryRegion::HeapHeader);
          // 新实例的实际类已由分配操作确定；封闭镜像没有后续字段迁移。
          builder_.CreateAssumption(builder_.CreateICmpEQ(
              count,
              builder_.getInt32(program_.layouts.classes.at(instruction.index).field_count)));
        }
        const auto& type = source_.value_types.at(instruction.dst);
        auto* record = EncodeValue(builder_, object, {.kind = Kind::Class, .name = {}});
        Store(instruction.dst, DecodeValue(builder_, record, type), type);
        return {};
      }
      auto helper = module_.getOrInsertFunction(runtime::kNativeNewCallableSymbol, status_type,
                                                pointer, word, pointer);
      status = builder_.CreateCall(helper, {context, index, result_});
      break;
    }
    case Opcode::NewArray:
    case Opcode::NewDynamicArray: {
      auto* length = instruction.opcode == Opcode::NewArray
                         ? builder_.getInt64(std::bit_cast<std::uint64_t>(instruction.immediate))
                         : Payload(instruction.lhs);
      if (instruction.opcode == Opcode::NewDynamicArray) {
        Check(builder_.CreateICmpSGE(length, builder_.getInt64(0)),
              ExecutionStatus::IndexOutOfBounds);
      }
      auto helper = module_.getOrInsertFunction(runtime::kNativeNewArraySymbol, pointer, pointer,
                                                word, builder_.getInt64Ty());
      auto& allocation = *llvm::cast<llvm::Function>(helper.getCallee());
      MarkHeapAllocation(allocation);
      auto* array = builder_.CreateCall(helper, {context, index, length});
      Check(builder_.CreateIsNotNull(array), ExecutionStatus::OutOfMemory);
      const auto& type = source_.value_types.at(instruction.dst);
      auto* record = EncodeValue(builder_, array, {.kind = Kind::Array, .name = {}});
      Store(instruction.dst, DecodeValue(builder_, record, type), type);
      auto* actual = LoadMemory(builder_.getInt32Ty(),
                                ByteAddress(array, runtime::NativeMemoryLayout::kObjectLength),
                                MemoryRegion::HeapHeader);
      builder_.CreateAssumption(
          builder_.CreateICmpEQ(builder_.CreateZExt(actual, builder_.getInt64Ty()), length));
      return {};
    }
    case Opcode::NewCollection: {
      auto helper = module_.getOrInsertFunction(runtime::kNativeNewCollectionSymbol, status_type,
                                                pointer, word, pointer);
      status = builder_.CreateCall(helper, {context, index, result_});
      break;
    }
    case Opcode::CallIndirect: {
      if (const auto target = CallTarget(instruction_index_)) {
        EmitCall(instruction, *target);
        return {};
      }
      const auto count = instruction.index;
      PackArguments(instruction.lhs, count, ArgumentPrefix::Implicit);
      auto* callable = Pack(Encode(instruction.rhs), static_cast<std::size_t>(count) + 1);
      auto helper =
          module_.getOrInsertFunction(runtime::kNativeCallIndirectSymbol, status_type, pointer,
                                      pointer, pointer, builder_.getInt64Ty(), pointer);
      status = builder_.CreateCall(
          helper, {context, callable, arguments_, builder_.getInt64(count), result_});
      break;
    }
    case Opcode::CallInterface: {
      const auto count = static_cast<std::size_t>(instruction.immediate);
      PackArguments(instruction.lhs, count);
      if (program_.native_mode == NativeProgramMode::Reloadable) {
        const auto selector = program_.selector_declarations.at(instruction.index);
        auto helper = module_.getOrInsertFunction(runtime::kNativeCallInterfaceV3Symbol,
                                                  status_type, pointer, builder_.getInt64Ty(),
                                                  pointer, builder_.getInt64Ty(), pointer);
        status = builder_.CreateCall(helper, {
                                                 context,
                                                 builder_.getInt64(selector.value),
                                                 arguments_,
                                                 builder_.getInt64(count),
                                                 result_,
                                             });
        break;
      }
      auto helper =
          module_.getOrInsertFunction(runtime::kNativeCallInterfaceV1Symbol, status_type, pointer,
                                      word, pointer, builder_.getInt64Ty(), pointer);
      status = builder_.CreateCall(helper,
                                   {context, index, arguments_, builder_.getInt64(count), result_});
      break;
    }
    case Opcode::GetGlobal:
    case Opcode::SetGlobal: {
      EmitGlobal(instruction);
      return {};
    }
    case Opcode::IsType:
    case Opcode::CheckedCast: {
      auto* value = Pack(Encode(instruction.lhs), 0);
      auto helper = module_.getOrInsertFunction(runtime::kNativeTypeTestSymbol, status_type,
                                                pointer, word, pointer);
      auto* matches = builder_.CreateCall(helper, {context, index, value});
      auto* condition = builder_.CreateICmpNE(matches, builder_.getInt8(0));
      if (instruction.opcode == Opcode::IsType) {
        Store(instruction.dst, condition, {.kind = Kind::Bool, .name = frontend::kBoolTypeName});
      } else {
        Check(condition, ExecutionStatus::InvalidCast);
        Store(instruction.dst, Load(instruction.lhs), source_.value_types.at(instruction.lhs));
      }
      return {};
    }
    case Opcode::Catch: {
      auto helper = module_.getOrInsertFunction(runtime::kNativeCatchSymbol, builder_.getVoidTy(),
                                                pointer, pointer);
      builder_.CreateCall(helper, {context, result_});
      Unpack(instruction.dst);
      return {};
    }
    case Opcode::ExceptionValue: {
      auto helper = module_.getOrInsertFunction(runtime::kNativeExceptionValueSymbol,
                                                builder_.getVoidTy(), pointer, pointer);
      builder_.CreateCall(helper, {Pack(Encode(instruction.lhs), 0), result_});
      Unpack(instruction.dst);
      return {};
    }
    case Opcode::Throw:
    case Opcode::Rethrow: {
      const auto symbol = instruction.opcode == Opcode::Throw ? runtime::kNativeThrowSymbol
                                                              : runtime::kNativeRethrowSymbol;
      auto helper = module_.getOrInsertFunction(symbol, status_type, pointer, pointer);
      Fail(builder_.CreateCall(helper, {context, Pack(Encode(instruction.lhs), 0)}));
      return {};
    }
    case Opcode::GetField:
    case Opcode::SetField:
      EmitField(instruction);
      return {};
    case Opcode::GetIndex:
    case Opcode::SetIndex:
      EmitIndex(instruction);
      return {};
    case Opcode::BuiltinCall:
      return EmitBuiltin(instruction);
    case Opcode::StringConcat: {
      auto* left = Pack(Encode(instruction.lhs), 0);
      auto* right = Pack(Encode(instruction.rhs), 1);
      auto helper = module_.getOrInsertFunction(runtime::kNativeConcatSymbol, status_type, pointer,
                                                pointer, pointer, pointer);
      status = builder_.CreateCall(helper, {context, left, right, result_});
      break;
    }
    case Opcode::EqualString:
    case Opcode::NotEqualString: {
      auto* left = Pack(Encode(instruction.lhs), 0);
      auto* right = Pack(Encode(instruction.rhs), 1);
      auto helper = module_.getOrInsertFunction(runtime::kNativeCompareStringsSymbol,
                                                builder_.getInt8Ty(), pointer, pointer);
      auto* compared =
          builder_.CreateICmpNE(builder_.CreateCall(helper, {left, right}), builder_.getInt8(0));
      if (instruction.opcode == Opcode::NotEqualString) {
        compared = builder_.CreateNot(compared);
      }
      Store(instruction.dst, compared, {.kind = Kind::Bool, .name = frontend::kBoolTypeName});
      return {};
    }
    case Opcode::StringInterpolate: {
      PackArguments(instruction.lhs, instruction.rhs);
      auto helper = module_.getOrInsertFunction(runtime::kNativeInterpolateSymbol, status_type,
                                                pointer, pointer, builder_.getInt64Ty(), pointer);
      status = builder_.CreateCall(
          helper, {context, arguments_, builder_.getInt64(instruction.rhs), result_});
      break;
    }
    case Opcode::Print: {
      auto* value = Pack(Encode(instruction.lhs), 0);
      auto helper = module_.getOrInsertFunction(runtime::kNativePrintSymbol, builder_.getVoidTy(),
                                                pointer, pointer);
      builder_.CreateCall(helper, {context, value});
      return {};
    }
    default:
      return std::unexpected("native lowering is not implemented for opcode " +
                             std::to_string(static_cast<unsigned>(instruction.opcode)));
  }
  CompleteCall(status, instruction.dst);
  return {};
}

}  // namespace vela::compiler::native
