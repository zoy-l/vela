#include "compiler/native/function_emitter.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/Support/Casting.h>

#include "base/builtin.h"
#include "base/callable.h"
#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "base/reflection.h"
#include "base/reflection_operation.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/integer_loop_optimizer.h"
#include "compiler/native/memory_regions.h"
#include "compiler/native/native_values.h"
#include "runtime/heap/instance_storage.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_memory_layout.h"
#include "runtime/reflection/reflection_handle.h"
#include "runtime/reflection/reflection_lookup_cache.h"
#include "runtime/value/object.h"

namespace vela::compiler::native {

void FunctionEmitter::EmitReflection(const ir::Instruction& instruction) {
  if (!reflection_operations_.empty()) {
    const auto operation = reflection_operations_[instruction_index_];
    if (operation == reflection::Operation::GetValue ||
        operation == reflection::Operation::SetValue) {
      EmitReflectedField(instruction, *operation);
      return;
    }
    if (operation == reflection::Operation::Invoke ||
        operation == reflection::Operation::InvokeStatic) {
      EmitReflectedCall(instruction, *operation);
      return;
    }
    if (operation && (operation == reflection::Operation::FindType ||
                      runtime::IsCachedMemberLookup(*operation))) {
      EmitReflectedLookup(instruction, *operation);
      return;
    }
  }
  EmitRuntimeBuiltin(instruction);
}

void FunctionEmitter::CacheReflectionMembers() {
  if (reflection_operations_.empty()) {
    return;
  }
  reflection_members_.resize(source_.instructions.size());
  for (std::size_t index = 0; index < source_.instructions.size(); ++index) {
    const auto operation = reflection_operations_[index];
    if (operation) {
      reflection_members_[index] = BoundReflectedMember(source_.instructions[index], *operation);
    }
  }
}

llvm::Value* FunctionEmitter::ReflectionCacheAddress(std::size_t offset) {
  const auto& instruction = source_.instructions[instruction_index_];
  auto* handle = LoadMemory(
      builder_.getPtrTy(), ByteAddress(Reference(instruction.lhs + 1), sizeof(runtime::HeapObject)),
      MemoryRegion::FieldPayloads);
  handle->setMetadata(
      llvm::LLVMContext::MD_tbaa,
      NativeMemoryTag(
          module_.getContext(), MemoryRegion::FieldPayloads,
          ClosedFieldDeclaration(program_, source_.value_types[instruction.lhs + 1].name, 0)));
  return ByteAddress(handle, sizeof(runtime::HeapObject) + offset);
}

llvm::Value* FunctionEmitter::IsReflectionCacheReady(llvm::Value* epoch) {
  // BeforeRuntime 的无请求结果可供本指令复用；处理过请求时仍须进入完整入口。
  auto* ready = program_.native_mode == NativeProgramMode::Reloadable
                    ? boundary_ready_
                    : builder_.CreateICmpEQ(LoadSafepointRequests(), builder_.getInt8(0));
  return builder_.CreateAnd(ready, builder_.CreateIsNotNull(epoch));
}

llvm::Value* FunctionEmitter::IsReflectionEpochCurrent(llvm::Value& epoch_address,
                                                       llvm::Value* epoch_observed) {
  auto* current = LoadMemory(builder_.getInt64Ty(), &epoch_address, MemoryRegion::ReflectionCache);
  return builder_.CreateICmpEQ(current, epoch_observed);
}

llvm::Value* FunctionEmitter::IsReflectionReceiver(ir::RegisterId receiver) {
  auto* kind = builder_.CreateExtractValue(Encode(receiver), 1);
  return builder_.CreateAnd(builder_.CreateIsNotNull(Reference(receiver)),
                            builder_.CreateICmpEQ(kind, builder_.getInt8(NativeKind(Kind::Class))));
}

void FunctionEmitter::GuardReflectionLayout(llvm::Value& receiver,
                                            llvm::Value* receiver_layout_id,
                                            llvm::BasicBlock& slow) {
  using runtime::NativeMemoryLayout;
  auto& context = module_.getContext();
  auto* layout =
      LoadMemory(builder_.getPtrTy(), ByteAddress(&receiver, NativeMemoryLayout::kObjectLayout),
                 MemoryRegion::HeapHeader);
  auto* identified = llvm::BasicBlock::Create(context, "reflection.identified", body_);
  builder_.CreateCondBr(builder_.CreateIsNotNull(layout), identified, &slow);
  builder_.SetInsertPoint(identified);
  // 缓存不保活布局；唯一 id 也能拒绝被回收后地址复用的布局。
  auto* actual =
      LoadMemory(builder_.getInt64Ty(), ByteAddress(layout, NativeMemoryLayout::kClassId),
                 MemoryRegion::ClassLayout);
  auto* matched = llvm::BasicBlock::Create(context, "reflection.layout.matched", body_);
  builder_.CreateCondBr(builder_.CreateICmpEQ(actual, receiver_layout_id), matched, &slow);
  builder_.SetInsertPoint(matched);
}

void FunctionEmitter::EmitReflectedLookup(const ir::Instruction& instruction,
                                          reflection::Operation operation) {
  using runtime::NativeMemoryLayout;
  using runtime::ReflectedLookupCache;
  const auto type_lookup = operation == reflection::Operation::FindType;
  auto& context = module_.getContext();
  auto* slow = llvm::BasicBlock::Create(context, "lookup.resolve", body_);
  auto* inspect = llvm::BasicBlock::Create(context, "lookup.epoch", body_);
  auto* matched = llvm::BasicBlock::Create(context, "lookup.matched", body_);
  auto* done = llvm::BasicBlock::Create(context, "lookup.done", body_);
  auto* program = ProgramData();
  llvm::Value* cache = nullptr;
  if (type_lookup) {
    cache = ByteAddress(program, NativeMemoryLayout::kProgramReflectionTypeCache);
  } else {
    cache = ReflectionCacheAddress(offsetof(runtime::ReflectionHandle, lookup_cache));
  }
  const auto load = [&](llvm::Type* type, std::size_t offset) {
    return LoadMemory(type, ByteAddress(cache, offset), MemoryRegion::ReflectionCache);
  };
  auto* epoch = load(builder_.getPtrTy(), offsetof(ReflectedLookupCache, epoch));
  auto* no_query = builder_.CreateIsNull(Reference(instruction.lhs + 3));
  if (operation == reflection::Operation::FindMethod ||
      operation == reflection::Operation::FindStaticMethod) {
    no_query = builder_.CreateAnd(no_query, builder_.CreateIsNull(Reference(instruction.lhs + 4)));
  }
  auto* usable = builder_.CreateAnd(IsReflectionCacheReady(epoch), no_query);
  auto* poll = builder_.CreateCondBr(usable, inspect, slow,
                                     llvm::MDBuilder(context).createLikelyBranchWeights());
  if (program_.native_mode != NativeProgramMode::Reloadable) {
    // 缓存快路不调用、不分配；请求检查沿用纯计算片段的有界轮询预算。
    poll->setMetadata(kNativeBoundedPollMetadata, llvm::MDNode::get(context, {}));
  }
  builder_.SetInsertPoint(inspect);
  auto* observed = load(builder_.getInt64Ty(), offsetof(ReflectedLookupCache, observed));
  llvm::Value* matches = IsReflectionEpochCurrent(*epoch, observed);
  matches = builder_.CreateAnd(
      matches, builder_.CreateICmpEQ(
                   program, load(builder_.getPtrTy(), offsetof(ReflectedLookupCache, program))));
  matches = builder_.CreateAnd(
      matches,
      builder_.CreateICmpEQ(load(builder_.getInt32Ty(), offsetof(ReflectedLookupCache, operation)),
                            builder_.getInt32(static_cast<std::uint32_t>(operation))));
  auto* result_type = builder_.CreateTrunc(Payload(instruction.lhs + kReflectionOperationArity - 1),
                                           builder_.getInt32Ty());
  matches = builder_.CreateAnd(
      matches,
      builder_.CreateICmpEQ(
          load(builder_.getInt32Ty(), offsetof(ReflectedLookupCache, result_type)), result_type));
  matches = builder_.CreateAnd(
      matches,
      builder_.CreateICmpEQ(load(builder_.getPtrTy(), offsetof(ReflectedLookupCache, name)),
                            Reference(instruction.lhs + 2)));
  if (type_lookup) {
    matches = builder_.CreateAnd(
        matches,
        builder_.CreateICmpEQ(load(builder_.getPtrTy(), offsetof(ReflectedLookupCache, qualifier)),
                              Reference(instruction.lhs + 1)));
  }
  builder_.CreateCondBr(matches, matched, slow,
                        llvm::MDBuilder(context).createLikelyBranchWeights());
  builder_.SetInsertPoint(matched);
  auto* record = load(NativeRecordType(context), offsetof(ReflectedLookupCache, result));
  const auto& type = source_.value_types[instruction.dst];
  Store(instruction.dst, DecodeValue(builder_, record, type), type);
  builder_.CreateBr(done);
  builder_.SetInsertPoint(slow);
  auto* status = CallRuntimeBuiltin(instruction);
  llvm::cast<llvm::Instruction>(status)->setMetadata(kNativeGuardFailureMetadata,
                                                     llvm::MDNode::get(context, {}));
  CompleteCall(status, instruction.dst);
  builder_.CreateBr(done);
  builder_.SetInsertPoint(done);
}

void FunctionEmitter::EmitReflectedField(const ir::Instruction& instruction,
                                         reflection::Operation operation) {
  using runtime::NativeMemoryLayout;
  using runtime::ReflectedFieldCache;
  const auto* member =
      reflection_members_.empty() ? nullptr : reflection_members_[instruction_index_];
  if (member != nullptr) {
    // 封闭映像的描述符来源、非空接收者类型和字段槽均已证明；字段存储沿用
    // 普通静态访问的布局契约，写入仍保留动态值检查和 GC 写屏障。
    auto* receiver = Reference(instruction.lhs + 2);
    const auto address = ObjectFieldAddress(receiver, member->target);
    const auto& result_type = source_.value_types[instruction.dst];
    if (operation == reflection::Operation::GetValue) {
      Store(instruction.dst, ReadStoredValue(address, result_type), result_type);
    } else {
      auto value = LoadStoredValue(instruction.lhs + 3);
      const auto& field_type = program_.runtime_types[member->type];
      if (field_type.kind != Kind::Dynamic) {
        auto* kind = builder_.CreateExtractValue(EncodeStoredValue(value), 1);
        auto* matches = builder_.CreateICmpEQ(kind, builder_.getInt8(NativeKind(field_type.kind)));
        if (field_type.nullable) {
          matches = builder_.CreateOr(
              matches, builder_.CreateICmpEQ(kind, builder_.getInt8(NativeKind(Kind::Null))));
        }
        Check(matches, ExecutionStatus::InvalidCast);
      }
      WriteStoredValue(address, value);
      EmitWriteBarrier(receiver, value);
      Store(instruction.dst, builder_.getFalse(), result_type);
    }
    return;
  }
  auto& context = module_.getContext();
  auto* slow = llvm::BasicBlock::Create(context, "reflection.resolve", body_);
  auto* inspect = llvm::BasicBlock::Create(context, "reflection.epoch", body_);
  auto* layout_matches = llvm::BasicBlock::Create(context, "reflection.layout", body_);
  auto* fixed = llvm::BasicBlock::Create(context, "reflection.inline", body_);
  auto* reloadable = llvm::BasicBlock::Create(context, "reflection.reloadable", body_);
  auto* ready = llvm::BasicBlock::Create(context, "reflection.ready", body_);
  auto* access = llvm::BasicBlock::Create(context, "reflection.access", body_);
  auto* done = llvm::BasicBlock::Create(context, "reflection.done", body_);
  auto* cache = ReflectionCacheAddress(offsetof(runtime::ReflectionHandle, field_cache));
  const auto load = [&](llvm::Type* type, std::size_t offset) {
    return LoadMemory(type, ByteAddress(cache, offset), MemoryRegion::ReflectionCache);
  };
  auto* epoch = load(builder_.getPtrTy(), offsetof(ReflectedFieldCache, epoch));
  auto* receiver = Reference(instruction.lhs + 2);
  auto* usable =
      builder_.CreateAnd(IsReflectionCacheReady(epoch), IsReflectionReceiver(instruction.lhs + 2));
  builder_.CreateCondBr(usable, inspect, slow,
                        llvm::MDBuilder(context).createLikelyBranchWeights());
  builder_.SetInsertPoint(inspect);
  auto* observed = load(builder_.getInt64Ty(), offsetof(ReflectedFieldCache, observed));
  builder_.CreateCondBr(IsReflectionEpochCurrent(*epoch, observed), layout_matches, slow);
  builder_.SetInsertPoint(layout_matches);
  auto* expected = load(builder_.getInt64Ty(), offsetof(ReflectedFieldCache, layout_id));
  GuardReflectionLayout(*receiver, expected, *slow);
  auto* indirect = load(builder_.getInt8Ty(), offsetof(ReflectedFieldCache, reloadable));
  builder_.CreateCondBr(builder_.CreateICmpNE(indirect, builder_.getInt8(0)), reloadable, fixed);
  builder_.SetInsertPoint(fixed);
  auto* inline_storage = ByteAddress(receiver, sizeof(runtime::HeapObject));
  builder_.CreateBr(access);
  builder_.SetInsertPoint(reloadable);
  auto* storage = LoadMemory(builder_.getPtrTy(),
                             ByteAddress(receiver, NativeMemoryLayout::kObjectNativePayload),
                             MemoryRegion::HeapHeader);
  auto* unready = LoadMemory(builder_.getInt32Ty(),
                             ByteAddress(storage, offsetof(runtime::InstanceStorage, unready)),
                             MemoryRegion::InstanceHeader);
  // 需要初始化时沿用当前发布的初始化器和 FieldState 契约。
  builder_.CreateCondBr(builder_.CreateICmpEQ(unready, builder_.getInt32(0)), ready, slow);
  builder_.SetInsertPoint(ready);
  auto* field_storage = ByteAddress(storage, sizeof(runtime::InstanceStorage));
  builder_.CreateBr(access);
  builder_.SetInsertPoint(access);
  auto* fields = builder_.CreatePHI(builder_.getPtrTy(), 2);
  fields->addIncoming(inline_storage, fixed);
  fields->addIncoming(field_storage, ready);
  auto* slot = load(builder_.getInt32Ty(), offsetof(ReflectedFieldCache, slot));
  auto* kind_offset = load(builder_.getInt64Ty(), offsetof(ReflectedFieldCache, kind_offset));
  const auto address = FieldAddress(fields, *slot, kind_offset);
  const auto& result_type = source_.value_types[instruction.dst];
  if (operation == reflection::Operation::GetValue) {
    Store(instruction.dst, ReadStoredValue(address, result_type), result_type);
  } else {
    auto value = LoadStoredValue(instruction.lhs + 3);
    auto* kind = builder_.CreateExtractValue(EncodeStoredValue(value), 1);
    auto* expected_kind = load(builder_.getInt8Ty(), offsetof(ReflectedFieldCache, kind));
    auto* nullable = load(builder_.getInt8Ty(), offsetof(ReflectedFieldCache, nullable));
    auto* dynamic = load(builder_.getInt8Ty(), offsetof(ReflectedFieldCache, dynamic));
    auto* matches = builder_.CreateOr(
        builder_.CreateICmpEQ(kind, expected_kind),
        builder_.CreateAnd(builder_.CreateICmpNE(nullable, builder_.getInt8(0)),
                           builder_.CreateICmpEQ(kind, builder_.getInt8(NativeKind(Kind::Null)))));
    Check(builder_.CreateOr(builder_.CreateICmpNE(dynamic, builder_.getInt8(0)), matches),
          ExecutionStatus::InvalidCast);
    WriteStoredValue(address, value);
    EmitWriteBarrier(receiver, value);
    Store(instruction.dst, builder_.getFalse(), result_type);
  }
  builder_.CreateBr(done);
  builder_.SetInsertPoint(slow);
  EmitRuntimeBuiltin(instruction);
  builder_.CreateBr(done);
  builder_.SetInsertPoint(done);
}

const reflection::Member* FunctionEmitter::BoundReflectedMember(
    const ir::Instruction& instruction,
    reflection::Operation operation) const {
  const auto field =
      operation == reflection::Operation::GetValue || operation == reflection::Operation::SetValue;
  if (!IsClosedLinkedProgram(program_.native_mode, placement_) ||
      (!field && operation != reflection::Operation::Invoke) || !instruction.reflection_member) {
    return nullptr;
  }
  const auto& member = program_.reflection_members[*instruction.reflection_member];
  const auto& receiver = source_.value_types[instruction.lhs + 2];
  const auto& owner = program_.runtime_types[member.declaring_type];
  if (member.kind != (field ? reflection::MemberKind::Field : reflection::MemberKind::Method) ||
      (member.flags & (reflection::kPrivate | reflection::kStatic)) != 0 ||
      member.target == kInvalidFunctionTarget || owner.kind != Kind::Class ||
      receiver.kind != Kind::Class || receiver.nullable || receiver.name != owner.name) {
    return nullptr;
  }
  if (field) {
    const auto kind = program_.runtime_types[member.type].kind;
    // 名义对象与容器仍由通用反射检查完整类型；enum 还需执行盒装转换。
    return kind == Kind::Dynamic || kind == Kind::Null || kind == Kind::Bool || kind == Kind::Int ||
                   kind == Kind::Double || kind == Kind::String
               ? &member
               : nullptr;
  }
  const auto& function = program_.functions[member.target];
  if (!CanInlineSmallFunction(function) || !IsScalar(function.return_type) ||
      function.return_type.kind == Kind::Enum ||
      std::ranges::any_of(member.signature.parameters, &CallSignature::Parameter::rest)) {
    return nullptr;
  }
  for (std::size_t index = 1; index < function.parameter_count; ++index) {
    const auto kind = function.value_types[index].kind;
    if (kind != Kind::Dynamic && kind != Kind::Bool && kind != Kind::Int && kind != Kind::Double &&
        kind != Kind::String) {
      return nullptr;
    }
  }
  return &member;
}

void FunctionEmitter::EmitReflectedCall(const ir::Instruction& instruction,
                                        reflection::Operation operation) {
  using runtime::NativeMemoryLayout;
  using runtime::ReflectedCallCache;
  const auto instance_call = operation == reflection::Operation::Invoke;
  const auto hidden = instance_call ? 1U : 0U;
  const auto snapshot = ir::IsBuiltinCall(instruction, Builtin::ReflectionInvoke);
  auto& context = module_.getContext();
  auto* slow = llvm::BasicBlock::Create(context, "reflection.resolve", body_);
  auto* signature = llvm::BasicBlock::Create(context, "reflection.signature", body_);
  auto* prepare = llvm::BasicBlock::Create(context, "reflection.prepare", body_);
  auto* done = llvm::BasicBlock::Create(context, "reflection.done", body_);
  const auto* bound =
      reflection_members_.empty() ? nullptr : reflection_members_[instruction_index_];
  const auto leaf = bound != nullptr && generated_[bound->target].leaf != nullptr;
  llvm::Value* receiver = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  llvm::Value* cache = nullptr;
  llvm::Value* expected_count = nullptr;
  llvm::Value* entry = nullptr;
  if (bound != nullptr) {
    // 描述符来源已由裁剪后的封闭程序证明；调用仍消费现有包装器的参数检查与错误契约。
    // 接收者的非空名义类型由 Typed IR 保证，不再借用可被其他调用覆盖的单槽缓存。
    receiver = Reference(instruction.lhs + 2);
    expected_count =
        builder_.getInt32(static_cast<std::uint32_t>(bound->signature.parameters.size()));
    entry = generated_[bound->target].reflection_entry;
    builder_.CreateBr(signature);
  } else {
    auto* inspect = llvm::BasicBlock::Create(context, "reflection.epoch", body_);
    cache = ReflectionCacheAddress(offsetof(runtime::ReflectionHandle, call_cache));
    auto* epoch =
        LoadMemory(builder_.getPtrTy(), ByteAddress(cache, offsetof(ReflectedCallCache, epoch)),
                   MemoryRegion::ReflectionCache);
    auto* usable = IsReflectionCacheReady(epoch);
    if (instance_call) {
      receiver = Reference(instruction.lhs + 2);
      usable = builder_.CreateAnd(usable, IsReflectionReceiver(instruction.lhs + 2));
    }
    builder_.CreateCondBr(usable, inspect, slow,
                          llvm::MDBuilder(context).createLikelyBranchWeights());
    builder_.SetInsertPoint(inspect);
    auto* observed = LoadMemory(builder_.getInt64Ty(),
                                ByteAddress(cache, offsetof(ReflectedCallCache, observed)),
                                MemoryRegion::ReflectionCache);
    auto* program =
        LoadMemory(builder_.getPtrTy(), ByteAddress(cache, offsetof(ReflectedCallCache, program)),
                   MemoryRegion::ReflectionCache);
    auto* matches = builder_.CreateAnd(IsReflectionEpochCurrent(*epoch, observed),
                                       builder_.CreateICmpEQ(ProgramData(), program));
    builder_.CreateCondBr(matches, signature, slow);
  }
  builder_.SetInsertPoint(signature);
  const auto load = [&](llvm::Type* type, std::size_t offset) {
    return LoadMemory(type, ByteAddress(cache, offset), MemoryRegion::ReflectionCache);
  };
  if (instance_call && bound == nullptr) {
    auto* expected = load(builder_.getInt64Ty(), offsetof(ReflectedCallCache, layout_id));
    GuardReflectionLayout(*receiver, expected, *slow);
  }
  llvm::Value* items = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  llvm::Value* values = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  llvm::Value* count = nullptr;
  if (snapshot) {
    const auto input_count = ir::CallArgumentCount(instruction) - kReflectionInvokeHeaderArity;
    for (std::size_t index = 0; index < input_count; ++index) {
      Pack(Encode(
               static_cast<ir::RegisterId>(instruction.lhs + kReflectionInvokeHeaderArity + index)),
           index);
    }
    values = arguments_;
    count = builder_.getInt32(static_cast<std::uint32_t>(input_count / 2));
  } else {
    auto* arguments = Reference(instruction.lhs + 2 + hidden);
    auto* loaded =
        builder_.CreateLoad(builder_.getPtrTy(), ObjectFieldAddress(arguments, 0).payload);
    loaded->setMetadata(
        llvm::LLVMContext::MD_tbaa,
        NativeMemoryTag(context, MemoryRegion::FieldPayloads,
                        ClosedFieldDeclaration(program_, reflection::kCallArgumentsName, 0)));
    items = loaded;
    count = LoadMemory(builder_.getInt32Ty(), ByteAddress(items, NativeMemoryLayout::kObjectLength),
                       MemoryRegion::HeapHeader);
  }
  if (bound == nullptr) {
    expected_count = load(builder_.getInt32Ty(), offsetof(ReflectedCallCache, count));
  }
  builder_.CreateCondBr(builder_.CreateICmpEQ(count, expected_count), prepare, slow);
  builder_.SetInsertPoint(prepare);
  if (bound == nullptr) {
    entry = load(builder_.getPtrTy(), offsetof(ReflectedCallCache, entry));
  }
  const std::array<llvm::Value*, 5> arguments{body_->getArg(0), receiver, items, values, result_};
  if (leaf) {
    auto unpacked = UnpackReflectionArguments(
        builder_, program_, program_.functions[bound->target],
        {.execution = body_->getArg(0), .receiver = receiver, .items = items, .snapshots = values},
        *slow);
    unpacked.push_back(leaf_result_);
    PrepareTraceFrame();
    EmitLeafCall(instruction, {.function = bound->target}, unpacked);
  } else {
    auto* call = llvm::BasicBlock::Create(context, "reflection.call", body_);
    auto* status = bound == nullptr
                       ? EmitReflectedCallTarget(instruction, operation, entry, arguments)
                       : builder_.CreateCall(generated_[bound->target].reflection_entry, arguments);
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(status, builder_.getInt8(runtime::kNativeReflectionCallMiss)), slow,
        call);
    builder_.SetInsertPoint(call);
    CompleteCall(status, instruction.dst);
  }
  builder_.CreateBr(done);
  builder_.SetInsertPoint(slow);
  if (leaf) {
    PrepareInlineRuntimeCall();
  }
  auto* status = CallRuntimeBuiltin(instruction);
  if (leaf) {
    llvm::cast<llvm::Instruction>(status)->setMetadata(kNativeGuardFailureMetadata,
                                                       llvm::MDNode::get(context, {}));
  }
  CompleteCall(status, instruction.dst);
  builder_.CreateBr(done);
  builder_.SetInsertPoint(done);
}

llvm::Value* FunctionEmitter::EmitReflectedCallTarget(const ir::Instruction& instruction,
                                                      reflection::Operation operation,
                                                      llvm::Value* entry,
                                                      std::span<llvm::Value* const> arguments) {
  auto* entry_type = llvm::FunctionType::get(builder_.getInt8Ty(),
                                             {
                                                 builder_.getPtrTy(),
                                                 builder_.getPtrTy(),
                                                 builder_.getPtrTy(),
                                                 builder_.getPtrTy(),
                                                 builder_.getPtrTy(),
                                             },
                                             false);
  const llvm::ArrayRef<llvm::Value*> inputs(arguments.data(), arguments.size());
  std::vector<llvm::Function*> candidates;
  const auto& receiver = source_.value_types[instruction.lhs + 2];
  if (IsClosedLinkedProgram(program_.native_mode, placement_) &&
      operation == reflection::Operation::Invoke && receiver.kind == Kind::Class) {
    std::size_t instructions = 0;
    for (const auto& member : program_.reflection_members) {
      if (member.kind != reflection::MemberKind::Method ||
          (member.flags & reflection::kStatic) != 0 || member.target == kInvalidFunctionTarget) {
        continue;
      }
      const auto& owner = program_.runtime_types[member.declaring_type];
      if (owner.kind != Kind::Class || owner.name != receiver.name) {
        continue;
      }
      const auto& function = program_.functions[member.target];
      auto* target = generated_[member.target].reflection_entry;
      if (function.value_types.front().name != receiver.name ||
          !target->hasFnAttribute(llvm::Attribute::AlwaysInline)) {
        continue;
      }
      instructions += function.instructions.size();
      if (instructions > kMaximumInlineInstructions) {
        candidates.clear();
        break;
      }
      candidates.push_back(target);
    }
  }
  if (candidates.empty()) {
    return builder_.CreateCall(entry_type, entry, inputs);
  }
  // 封闭映像的入口身份不会改变；精确匹配后保留同一包装器的完整参数与失败契约。
  // 直接边让 LLVM 合并拆包、字段操作和结果编码；未知目标仍通过原来的反射 ABI 调用。
  auto& context = module_.getContext();
  auto* merged = llvm::BasicBlock::Create(context, "reflection.dispatched", body_);
  auto* status = llvm::PHINode::Create(
      builder_.getInt8Ty(), static_cast<unsigned>(candidates.size() + 1), "status", merged);
  for (auto* candidate : candidates) {
    auto* direct = llvm::BasicBlock::Create(context, "reflection.direct", body_);
    auto* next = llvm::BasicBlock::Create(context, "reflection.indirect", body_);
    builder_.CreateCondBr(builder_.CreateICmpEQ(entry, candidate), direct, next);
    builder_.SetInsertPoint(direct);
    auto* result = builder_.CreateCall(candidate, inputs);
    builder_.CreateBr(merged);
    status->addIncoming(result, direct);
    builder_.SetInsertPoint(next);
  }
  auto* result = builder_.CreateCall(entry_type, entry, inputs);
  status->addIncoming(result, builder_.GetInsertBlock());
  builder_.CreateBr(merged);
  builder_.SetInsertPoint(merged);
  return status;
}

}  // namespace vela::compiler::native
