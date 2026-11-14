#include "compiler/aot/native_compiler.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Casting.h>

#include "base/builtin.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/aot/native_reflection_optimizer.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/lowerer.h"
#include "compiler/ir/preparation.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/field_access.h"
#include "compiler/native/native_roots.h"
#include "compiler/test_support/native_pass_test_support.h"
#include "compiler/test_support/preparation_test_support.h"
#include "compiler/test_support/test_sources.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_memory_layout.h"

namespace vela::compiler::aot {
namespace {

using ir::test::Prepare;
using native::test::ParseModule;
using native::test::WithFunctionLoops;

TEST(NativeCompilerTest, BindsOnlyImmutableClosedReflectionDescriptorSources) {
  auto prepared = Prepare(R"(
[Reflect]
export class Counter {
  value: int = 0;
  add(value: int): int { this.value = value; return value; }
  twice(value: int): int { return value * 2; }
}
export class Derived extends Counter {}
const info = Reflect.typeInfo<Counter>();
const cached = info.findMethod('add');
const module = info.module;
let selected = cached;
let selectedField = info.findField('value');
export func replace(): void {
  selected = info.findMethod('twice');
  selectedField = info.findField('missing');
}
export func known(object: Counter, value: int): dynamic {
  const method = cached;
  if (method == null) { return null; }
  return method.invoke(object, CallArguments(items: [CallArgument(value: value)]));
}
export func queried(object: Counter, value: int): dynamic {
  const qualifier = module;
  if (qualifier == null) { return null; }
  const selectedType = Reflect.findType(qualifier, 'Counter');
  if (selectedType == null) { return null; }
  const method = selectedType.findMethod('add');
  if (method == null) { return null; }
  return method.invoke(object, CallArguments(items: [CallArgument(value: value)]));
}
export func mutableGlobal(object: Counter, value: int): dynamic {
  const method = selected;
  if (method == null) { return null; }
  return method.invoke(object, CallArguments(items: [CallArgument(value: value)]));
}
func parameter(method: MethodInfo<Counter>, object: Counter): dynamic {
  return method.invoke(object, CallArguments());
}
export func knownField(object: Counter, value: int): dynamic {
  const field = info.findField('value');
  if (field == null) { return null; }
  field.setValue(object, value);
  return field.getValue(object);
}
export func mutableField(object: Counter, value: int): dynamic {
  const field = selectedField;
  if (field == null) { return null; }
  field.setValue(object, value);
  return field.getValue(object);
}
func parameterField(field: FieldInfo<Counter>, object: Counter): dynamic {
  return field.getValue(object);
}
export func inherited(object: Derived): dynamic {
  const method = Reflect.typeInfo<Derived>().findMethod('add');
  if (method == null) { return null; }
  return method.invoke(object, CallArguments(items: [CallArgument(value: 4)]));
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  auto& program = prepared.program;
  BindNativeReflectedMembers(program);
  for (const auto& [name, expected] : std::array{
           std::pair{"known", 1},
           std::pair{"queried", 1},
           std::pair{"mutableGlobal", 0},
           std::pair{"parameter", 0},
           std::pair{"knownField", 2},
           std::pair{"mutableField", 0},
           std::pair{"parameterField", 0},
           std::pair{"inherited", 0},
       }) {
    const auto found = std::ranges::find(program.functions, name, &ir::Function::name);
    ASSERT_NE(found, program.functions.end());
    const auto bound = std::ranges::count_if(found->instructions, [](const auto& instruction) {
      return (ir::IsBuiltinCall(instruction, Builtin::Reflection) ||
              ir::IsBuiltinCall(instruction, Builtin::ReflectionInvoke)) &&
             instruction.reflection_member;
    });
    EXPECT_EQ(bound, expected) << name;
  }
  const auto compiled = Compile(program, {
                                             .exports =
                                                 {
                                                     "known",
                                                     "queried",
                                                     "mutableGlobal",
                                                     "knownField",
                                                     "mutableField",
                                                     "inherited",
                                                     "replace",
                                                 },
                                         });
  ASSERT_TRUE(compiled) << frontend::DiagnosticsText(compiled.error());
}

ir::LoweringResult PrepareReloadable(std::string_view source) {
  auto prepared = Prepare(source);
  prepared.program.native_mode = NativeProgramMode::Reloadable;
  return prepared;
}

std::expected<native::NativeModule, frontend::Diagnostics> GenerateForTest(
    const ir::Program& program,
    const std::string& name,
    NativeCodePlacement placement,
    NativeCodeTier tier = NativeCodeTier::Optimized,
    native::NativeDebugging debugging = native::NativeDebugging::Safepoints) {
  const auto calls = native::PlanNativeCalls(program, tier, debugging);
  const auto fields = native::FindFieldTargets(program);
  return native::Generate(program, calls, fields, name, debugging, tier,
                          native::EscapeOptimization::Enabled, placement);
}

struct RangeSpecializationCase final {
  std::string_view source;
  std::string_view module_name;
  std::string_view function_name;
};

template <typename Verify>
void WithGeneratedRangeFunctions(const RangeSpecializationCase& scenario, Verify verify) {
  const auto prepared = Prepare(scenario.source);
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& functions = prepared.program.functions;
  const auto found = std::ranges::find(functions, scenario.function_name, &ir::Function::name);
  ASSERT_NE(found, functions.end());
  const auto name =
      std::string(scenario.module_name) + "_body_" + std::to_string(found - functions.begin());
  const auto generated = GenerateForTest(prepared.program, std::string(scenario.module_name),
                                         NativeCodePlacement::Linked);
  ASSERT_TRUE(generated) << frontend::DiagnosticsText(generated.error());
  const auto& module = *generated->module;
  const auto* generic = module.getFunction(name);
  const auto* specialized = module.getFunction(name + "_range");
  ASSERT_NE(generic, nullptr);
  ASSERT_NE(specialized, nullptr);
  verify(prepared.program, *generic, *specialized, name);
}

TEST(NativeCompilerTest, ConvertsCachedStringConstantsToDynamicFieldRegisters) {
  const auto prepared = Prepare(R"(
class Box { value: dynamic = null; }
export func read(): dynamic {
  const box = Box();
  box.value = 'cached';
  return box.value;
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"read"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->llvm_ir.find("@vela_native_v1_constant"), std::string::npos);
}

TEST(NativeCompilerTest, ReservesLeafResultsOnlyForCallsThatUseTheLeafEntry) {
  auto prepared = PrepareReloadable(R"(
func step(value: int): int { return value + 1; }
export func cold(value: int): int { return step(value); }
export func repeated(value: int): int {
  let sum = 0;
  for (let index = 0; index < 4; index++) { sum += step(value + index); }
  return sum;
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  auto& program = prepared.program;
  const auto& functions = program.functions;
  const auto cold = std::ranges::find(functions, "cold", &ir::Function::name);
  const auto repeated = std::ranges::find(functions, "repeated", &ir::Function::name);
  ASSERT_NE(cold, functions.end());
  ASSERT_NE(repeated, functions.end());
  const auto step = std::ranges::find(functions, "step", &ir::Function::name);
  ASSERT_NE(step, functions.end());
  EXPECT_EQ(native::PlanNonrecursiveLeaf(*step, native::NativeDebugging::Safepoints),
            native::LeafKind::Bounded);
  const auto calls = native::PlanNativeCalls(program, NativeCodeTier::Optimized);
  const auto repeated_function = static_cast<std::size_t>(repeated - functions.begin());
  const native::CallEdge repeated_edge{
      .caller = repeated_function,
      .callee = static_cast<std::size_t>(step - functions.begin()),
  };
  EXPECT_FALSE(calls.IsEntrySlotRequired(repeated_edge));
  const auto& dependencies = calls.dependencies.at(repeated_edge.caller);
  EXPECT_NE(std::ranges::find(dependencies, repeated_edge.callee), dependencies.end());
  const auto generated = GenerateForTest(program, "leaf_frames", NativeCodePlacement::Dynamic);
  ASSERT_TRUE(generated) << frontend::DiagnosticsText(generated.error());
  const auto cold_index = std::to_string(cold - functions.begin());
  const auto repeated_index = std::to_string(repeated_function);
  const auto& module = *generated->module;
  for (const auto& [name, expected] : std::array{
           std::pair{"leaf_frames_body_" + repeated_index, false},
           std::pair{"leaf_frames_" + cold_index + "_optimized", false},
           std::pair{"leaf_frames_" + repeated_index + "_optimized", true},
       }) {
    const auto* body = module.getFunction(name);
    ASSERT_NE(body, nullptr) << name;
    EXPECT_EQ(std::ranges::any_of(
                  llvm::instructions(body),
                  [](const auto& instruction) { return instruction.getName() == "leaf.result"; }),
              expected)
        << name;
  }
}

TEST(NativeCompilerTest, BorrowedReceiversKeepTheirEntryWhenScalarFieldLeavesExist) {
  const auto prepared = Prepare(R"(
class Counter {
  value: int;
  next(): int { this.value++; return this.value; }
}
export func calculate(initial: int): int {
  const counter = Counter(value: initial);
  return counter.next() + counter.value;
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& program = prepared.program;
  const auto& functions = program.functions;
  const auto caller = std::ranges::find(functions, "calculate", &ir::Function::name);
  ASSERT_NE(caller, functions.end());
  const auto method = std::ranges::find_if(caller->instructions, [&](const auto& instruction) {
    return instruction.opcode == Opcode::Call &&
           functions.at(instruction.index).return_type.kind == frontend::TypeDescriptor::Kind::Int;
  });
  ASSERT_NE(method, caller->instructions.end());
  const auto generated = GenerateForTest(program, "borrowed_leaf", NativeCodePlacement::Linked,
                                         NativeCodeTier::Baseline);
  ASSERT_TRUE(generated) << frontend::DiagnosticsText(generated.error());
  const auto& module = *generated->module;
  const auto ordinal = std::to_string(method->index);
  const auto* borrowed = module.getFunction("borrowed_leaf_borrowed_" + ordinal);
  const auto* leaf = module.getFunction("borrowed_leaf_" + ordinal + "_leaf");
  ASSERT_NE(borrowed, nullptr);
  ASSERT_NE(leaf, nullptr);
  const auto* body =
      module.getFunction("borrowed_leaf_body_" + std::to_string(caller - functions.begin()));
  ASSERT_NE(body, nullptr);
  const auto instructions = llvm::instructions(body);
  EXPECT_TRUE(std::ranges::any_of(instructions, [&](const auto& instruction) {
    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
    return call != nullptr && call->getCalledFunction() == borrowed;
  }));
  EXPECT_FALSE(std::ranges::any_of(instructions, [&](const auto& instruction) {
    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
    return call != nullptr && call->getCalledFunction() == leaf;
  }));
  EXPECT_FALSE(std::ranges::any_of(instructions, [](const auto& instruction) {
    return instruction.getName() == "leaf.result";
  }));
}

TEST(NativeCompilerTest, ReservesDistinctLeafResultsForBoundReflectedCalls) {
  auto prepared = Prepare(R"(
[Reflect]
class Counter {
  value: int = 0;
  add(value: int): int { this.value = value; return value; }
}
const info = Reflect.typeInfo<Counter>();
export func bound(object: Counter, value: int): dynamic {
  const method = info.findMethod('add');
  if (method == null) { return null; }
  return method.invoke(object, CallArguments(items: [CallArgument(value: value)]));
}
export func unbound(method: MethodInfo<Counter>, object: Counter, value: int): dynamic {
  return method.invoke(object, CallArguments(items: [CallArgument(value: value)]));
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  auto& program = prepared.program;
  BindNativeReflectedMembers(program);
  const auto generated = GenerateForTest(program, "reflected_results", NativeCodePlacement::Linked);
  ASSERT_TRUE(generated) << frontend::DiagnosticsText(generated.error());
  const auto& functions = program.functions;
  for (const auto& [name, expected] :
       std::array{std::pair{"bound", true}, std::pair{"unbound", false}}) {
    const auto found = std::ranges::find(functions, name, &ir::Function::name);
    ASSERT_NE(found, functions.end());
    const auto* body = generated->module->getFunction("reflected_results_body_" +
                                                      std::to_string(found - functions.begin()));
    ASSERT_NE(body, nullptr) << name;
    EXPECT_EQ(std::ranges::any_of(
                  llvm::instructions(body),
                  [](const auto& instruction) { return instruction.getName() == "leaf.result"; }),
              expected)
        << name;
  }
}

TEST(NativeCompilerTest, LeafEntriesConsumeCallerDepthAndPreserveRecursivePrepayment) {
  const auto prepared = Prepare(R"(
func increment(value: int): int { return value + 1; }
func read(values: int[], index: int): int { return values[index]; }
func recurse(value: int): int {
  if (value <= 0) { return 0; }
  return value + recurse(value - 1);
}
export func calculate(values: int[]): int {
  let sum = 0;
  for (let index = 0; index < 4; index++) {
    sum += increment(read(values, index)) + recurse(index);
  }
  return sum;
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto generated =
      GenerateForTest(prepared.program, "leaf_depth", NativeCodePlacement::Linked);
  ASSERT_TRUE(generated) << frontend::DiagnosticsText(generated.error());
  const auto& module = *generated->module;
  const auto& functions = prepared.program.functions;
  for (const auto* name : {"increment", "recurse"}) {
    const auto source = std::ranges::find(functions, name, &ir::Function::name);
    ASSERT_NE(source, functions.end());
    const auto* leaf =
        module.getFunction("leaf_depth_" + std::to_string(source - functions.begin()) + "_leaf");
    ASSERT_NE(leaf, nullptr) << name;
    const auto depth_index = static_cast<unsigned>(source->parameter_count + 1);
    const auto* depth = leaf->getArg(depth_index);
    EXPECT_TRUE(depth->getType()->isIntegerTy(64));
    EXPECT_FALSE(depth->use_empty());
    for (const auto& instruction : llvm::instructions(leaf)) {
      const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (load == nullptr) {
        continue;
      }
      std::int64_t offset = 0;
      const auto* base = llvm::GetPointerBaseWithConstantOffset(load->getPointerOperand(), offset,
                                                                module.getDataLayout());
      EXPECT_FALSE(base == leaf->getArg(0) &&
                   std::cmp_equal(offset, runtime::NativeMemoryLayout::kExecutionDepth));
    }
    std::size_t calls = 0;
    for (const auto* user : leaf->users()) {
      const auto* call = llvm::dyn_cast<llvm::CallBase>(user);
      if (call == nullptr || call->getCalledFunction() != leaf) {
        continue;
      }
      ++calls;
      const auto* passed = call->getArgOperand(depth_index);
      if (call->getFunction() == leaf) {
        EXPECT_EQ(passed, depth);
      } else {
        const auto* next = llvm::dyn_cast<llvm::BinaryOperator>(passed);
        ASSERT_NE(next, nullptr);
        EXPECT_EQ(next->getOpcode(), llvm::Instruction::Add);
        const auto* one = llvm::dyn_cast<llvm::ConstantInt>(next->getOperand(1));
        ASSERT_NE(one, nullptr);
        EXPECT_TRUE(one->isOne());
      }
    }
    EXPECT_GT(calls, 0U) << name;
  }
}

class ReadonlyLeafInliningTest
    : public testing::TestWithParam<std::tuple<bool, NativeCodeTier, native::NativeDebugging>> {};

TEST_P(ReadonlyLeafInliningTest, KeepsGuardedBodiesSeparateAndRespectsTierAndDebugging) {
  const auto [large, tier, debugging] = GetParam();
  auto prepared =
      PrepareReloadable("func kernel(values: int[], index: int): int { return values[index]; }");
  ASSERT_TRUE(prepared.Succeeded());
  auto& program = prepared.program;
  auto& functions = program.functions;
  const auto kernel = std::ranges::find(functions, "kernel", &ir::Function::name);
  ASSERT_NE(kernel, functions.end());
  auto& instructions = kernel->instructions;
  ASSERT_LE(instructions.size(), native::kMaximumInlineInstructions);
  if (large) {
    const auto read = std::ranges::find(instructions, Opcode::GetIndex, &ir::Instruction::opcode);
    ASSERT_NE(read, instructions.end());
    instructions.insert(read, native::kMaximumInlineInstructions + 1 - instructions.size(), *read);
  }
  const auto index = std::to_string(kernel - functions.begin());
  const auto generated =
      GenerateForTest(program, "readonly_inlining", NativeCodePlacement::Dynamic, tier, debugging);
  ASSERT_TRUE(generated) << frontend::DiagnosticsText(generated.error());
  const auto& module = *generated->module;
  const auto guarded =
      tier == NativeCodeTier::Optimized && debugging == native::NativeDebugging::Safepoints;
  const auto* body = module.getFunction("readonly_inlining_body_" + index);
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(body->hasFnAttribute(llvm::Attribute::AlwaysInline), !large && !guarded);
  const auto* leaf = module.getFunction("readonly_inlining_" + index + "_leaf");
  EXPECT_EQ(leaf != nullptr, guarded);
  if (leaf != nullptr) {
    EXPECT_TRUE(leaf->hasFnAttribute(llvm::Attribute::AlwaysInline));
  }
}

INSTANTIATE_TEST_SUITE_P(EntryModes,
                         ReadonlyLeafInliningTest,
                         testing::Combine(testing::Bool(),
                                          testing::Values(NativeCodeTier::Baseline,
                                                          NativeCodeTier::Optimized),
                                          testing::Values(native::NativeDebugging::Safepoints,
                                                          native::NativeDebugging::SourceLines,
                                                          native::NativeDebugging::Instructions)));

TEST(NativeCompilerTest, ReadonlyManagedLoadsCannotCollectBeforeReturningToTheirCaller) {
  auto prepared = PrepareReloadable(R"(
func nested(values: int[][], index: int): int { return values[index][0]; }
func discarded(values: string[], index: int): int { const unused = values[index]; return 42; }
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  auto& program = prepared.program;
  const auto generated = GenerateForTest(program, "managed_read", NativeCodePlacement::Dynamic);
  ASSERT_TRUE(generated) << frontend::DiagnosticsText(generated.error());
  const auto& functions = program.functions;
  for (const auto* name : {"nested", "discarded"}) {
    const auto found = std::ranges::find(functions, name, &ir::Function::name);
    ASSERT_NE(found, functions.end());
    EXPECT_EQ(native::PlanNonrecursiveLeaf(*found, native::NativeDebugging::Safepoints),
              native::LeafKind::Guarded);
    EXPECT_TRUE(std::ranges::any_of(found->instructions, [&](const auto& instruction) {
      using Kind = frontend::TypeDescriptor::Kind;
      const auto kind = found->value_types.at(instruction.dst).kind;
      return instruction.opcode == Opcode::GetIndex &&
             (kind == Kind::Array || kind == Kind::String);
    }));
    const auto* leaf = generated->module->getFunction(
        "managed_read_" + std::to_string(found - functions.begin()) + "_leaf");
    ASSERT_NE(leaf, nullptr);
    for (const auto& instruction : llvm::instructions(leaf)) {
      if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
        const auto* target = call->getCalledFunction();
        ASSERT_NE(target, nullptr);
        EXPECT_TRUE(target->isIntrinsic()) << target->getName().str();
      }
    }
  }
}

TEST(NativeCompilerTest, StackRootCleanupRestoresTheHeadLoadedOnEntry) {
  auto prepared = PrepareReloadable("func length(values: int[]): int { return values.length; }");
  ASSERT_TRUE(prepared.Succeeded());
  auto& program = prepared.program;
  const auto& functions = program.functions;
  const auto found = std::ranges::find(functions, "length", &ir::Function::name);
  ASSERT_NE(found, functions.end());
  const auto generated = GenerateForTest(program, "root_restore", NativeCodePlacement::Dynamic,
                                         NativeCodeTier::Baseline);
  ASSERT_TRUE(generated) << frontend::DiagnosticsText(generated.error());
  const auto* body = generated->module->getFunction("root_restore_body_" +
                                                    std::to_string(found - functions.begin()));
  ASSERT_NE(body, nullptr);
  const auto cleanup = std::ranges::find_if(
      *body, [](const auto& block) { return block.getName() == "frame.restored"; });
  ASSERT_NE(cleanup, body->end());
  const auto restore = std::ranges::find_if(
      *cleanup, [](const auto& instruction) { return llvm::isa<llvm::StoreInst>(instruction); });
  ASSERT_NE(restore, cleanup->end());
  const auto& store = llvm::cast<llvm::StoreInst>(*restore);
  const auto* previous = llvm::dyn_cast<llvm::LoadInst>(store.getValueOperand());
  ASSERT_NE(previous, nullptr);
  EXPECT_EQ(previous->getPointerOperand(), store.getPointerOperand());
  EXPECT_NE(previous->getParent(), &*cleanup);
}

TEST(NativeCompilerTest, DelayedStackRootCleanupRestoresTheHeadOnlyAfterRegistration) {
  auto prepared = PrepareReloadable("func length(values: int[]): int { return values.length; }");
  ASSERT_TRUE(prepared.Succeeded());
  const auto& program = prepared.program;
  const auto& functions = program.functions;
  const auto found = std::ranges::find(functions, "length", &ir::Function::name);
  ASSERT_NE(found, functions.end());
  const auto generated = GenerateForTest(program, "delayed_roots", NativeCodePlacement::Dynamic);
  ASSERT_TRUE(generated) << frontend::DiagnosticsText(generated.error());
  const auto& module = *generated->module;
  const auto* body =
      module.getFunction("delayed_roots_body_" + std::to_string(found - functions.begin()));
  ASSERT_NE(body, nullptr);
  const auto initialize =
      std::ranges::find(*body, llvm::StringRef("frame.initialize"), &llvm::BasicBlock::getName);
  const auto restore =
      std::ranges::find(*body, llvm::StringRef("frame.restore"), &llvm::BasicBlock::getName);
  const auto restored =
      std::ranges::find(*body, llvm::StringRef("frame.restored"), &llvm::BasicBlock::getName);
  ASSERT_NE(initialize, body->end());
  ASSERT_NE(restore, body->end());
  ASSERT_NE(restored, body->end());
  const auto link = std::ranges::find_if(*initialize, [&](const auto& instruction) {
    const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
    const auto* head =
        store != nullptr ? llvm::dyn_cast<llvm::LoadInst>(store->getPointerOperand()) : nullptr;
    if (head == nullptr) {
      return false;
    }
    std::int64_t offset = 0;
    const auto* base = llvm::GetPointerBaseWithConstantOffset(head->getPointerOperand(), offset,
                                                              module.getDataLayout());
    return base == body->getArg(0) &&
           std::cmp_equal(offset, runtime::NativeMemoryLayout::kExecutionStackRoots);
  });
  ASSERT_NE(link, initialize->end());
  const auto* head = llvm::cast<llvm::StoreInst>(*link).getPointerOperand();
  const auto save = std::ranges::find_if(*initialize, [&](const auto& instruction) {
    const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
    const auto* previous =
        store != nullptr ? llvm::dyn_cast<llvm::LoadInst>(store->getValueOperand()) : nullptr;
    return previous != nullptr && previous->getPointerOperand() == head;
  });
  ASSERT_NE(save, initialize->end());
  const auto cleanup = std::ranges::find_if(*restore, [&](const auto& instruction) {
    const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
    return store != nullptr && store->getPointerOperand() == head;
  });
  ASSERT_NE(cleanup, restore->end());
  const auto* previous =
      llvm::dyn_cast<llvm::LoadInst>(llvm::cast<llvm::StoreInst>(*cleanup).getValueOperand());
  ASSERT_NE(previous, nullptr);
  std::int64_t loaded_offset = 0;
  std::int64_t stored_offset = 0;
  const auto* loaded_base = llvm::GetPointerBaseWithConstantOffset(
      previous->getPointerOperand(), loaded_offset, module.getDataLayout());
  const auto* stored_base =
      llvm::GetPointerBaseWithConstantOffset(llvm::cast<llvm::StoreInst>(*save).getPointerOperand(),
                                             stored_offset, module.getDataLayout());
  EXPECT_EQ(loaded_base, stored_base);
  EXPECT_EQ(loaded_offset, stored_offset);
  const auto* predecessor = restore->getSinglePredecessor();
  ASSERT_NE(predecessor, nullptr);
  const auto* guard = llvm::dyn_cast<llvm::CondBrInst>(predecessor->getTerminator());
  ASSERT_NE(guard, nullptr);
  EXPECT_EQ(guard->getSuccessor(0), &*restore);
  EXPECT_EQ(guard->getSuccessor(1), &*restored);
}

TEST(NativeCompilerTest, EmitsOfflineObjectWithScalarCallsAndControlFlow) {
  const auto prepared = Prepare(R"(
func twice(value: int): int { return value * 2; }
export func main(): int {
  let sum = 0;
  for (let index = 0; index < 10; index++) { sum += twice(index); }
  return sum;
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"main"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  EXPECT_FALSE(result->object.empty());
  EXPECT_EQ(result->target, runtime::NativeTargetIdentity());
  EXPECT_NE(result->llvm_ir.find("vela_aot_entry_"), std::string::npos);
  ASSERT_EQ(result->exports.size(), 1U);
  EXPECT_EQ(result->exports.front().symbol, "vela_aot_export_1");
  EXPECT_NE(result->header.find("VelaAotStatus vela_aot_export_1"), std::string::npos);
  EXPECT_NE(result->header.find("Call_main"), std::string::npos);
  EXPECT_EQ(result->instantiate_symbol, "vela_aot_instantiate");
}

TEST(NativeCompilerTest, RestrictsRangeSpecializationsToLinkedAotBodies) {
  WithGeneratedRangeFunctions(
      {
          .source = R"(
func read(values: int[], index: int): int { return values[index] + 1; }
export func run(): int { return read([1, 2, 3], 2); }
)",
          .module_name = "ranges",
          .function_name = "read",
      },
      [](const auto& program, const auto& generic, const auto& specialized, const auto& name) {
        const auto checked_adds = [](const llvm::Function& function) {
          return std::ranges::count_if(llvm::instructions(function), [](const auto& instruction) {
            const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
            const auto* callee = call == nullptr ? nullptr : call->getCalledFunction();
            return callee != nullptr && callee->getName().starts_with("llvm.sadd.with.overflow");
          });
        };
        EXPECT_EQ(checked_adds(generic), 1);
        EXPECT_EQ(checked_adds(specialized), 0);
        EXPECT_FALSE(generic.hasParamAttribute(2, llvm::Attribute::Range));
        ASSERT_TRUE(specialized.hasParamAttribute(2, llvm::Attribute::Range));
        const auto& range =
            specialized.getAttributes().getParamAttr(2, llvm::Attribute::Range).getRange();
        EXPECT_EQ(range.getLower().getSExtValue(), 2);
        EXPECT_EQ(range.getUpper().getSExtValue(), 3);
        const auto dynamic = GenerateForTest(program, "ranges", NativeCodePlacement::Dynamic);
        ASSERT_TRUE(dynamic) << frontend::DiagnosticsText(dynamic.error());
        EXPECT_EQ(dynamic->module->getFunction(name + "_range"), nullptr);
      });
}

TEST(NativeCompilerTest, BorrowsTheEntryProgramOnceWithoutMarkingExecutionImmutable) {
  const auto prepared = Prepare(R"(
[Reflect]
export class Counter { value: int = 0; }
let count = 0;
export func run(): int {
  const selectedType = Reflect.findType('main.vela', 'Counter');
  if (selectedType == null) { return -1; }
  const field = selectedType.findField('value');
  if (field == null) { return -1; }
  count++;
  return count;
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& functions = prepared.program.functions;
  const auto source = std::ranges::find(functions, "run", &ir::Function::name);
  ASSERT_NE(source, functions.end());
  const auto generated =
      GenerateForTest(prepared.program, "program_borrow", NativeCodePlacement::Linked);
  ASSERT_TRUE(generated) << frontend::DiagnosticsText(generated.error());
  const auto& module = *generated->module;
  const auto* body =
      module.getFunction("program_borrow_body_" + std::to_string(source - functions.begin()));
  ASSERT_NE(body, nullptr);
  std::size_t instance_loads = 0;
  for (const auto& instruction : llvm::instructions(body)) {
    const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
    if (load == nullptr) {
      continue;
    }
    std::int64_t offset = 0;
    const auto* base = llvm::GetPointerBaseWithConstantOffset(load->getPointerOperand(), offset,
                                                              module.getDataLayout());
    if (base == body->getArg(0) &&
        std::cmp_equal(offset, runtime::NativeMemoryLayout::kExecutionInstance)) {
      ++instance_loads;
      EXPECT_EQ(load->getMetadata(llvm::LLVMContext::MD_invariant_load), nullptr);
    }
  }
  EXPECT_EQ(instance_loads, 1U);
}

TEST(NativeCompilerTest, ProvenMutableGlobalAccessesDoNotAllocateReadinessSlots) {
  const auto prepared = Prepare(R"(
let value = 0;
export func run(input: int): int { value = input; return value; }
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto generated = GenerateForTest(prepared.program, "globals", NativeCodePlacement::Linked);
  ASSERT_TRUE(generated) << frontend::DiagnosticsText(generated.error());
  for (const auto& function : *generated->module) {
    for (const auto& instruction : llvm::instructions(function)) {
      EXPECT_FALSE(instruction.getName().starts_with("global.is_ready"));
    }
  }
}

TEST(NativeCompilerTest, FixedArrayLengthsKeepBoundsProofsOutsideTheGenericEntry) {
  WithGeneratedRangeFunctions(
      {
          .source = R"(
func read(values: int[]): int { return values[0]; }
export func run(): int { return read([-9223372036854775808, 9223372036854775807]); }
)",
          .module_name = "lengths",
          .function_name = "read",
      },
      [](const auto&, const auto& generic, const auto& specialized, const auto&) {
        const auto bounds_checks = [](const llvm::Function& function) {
          return std::ranges::count_if(llvm::instructions(function), [](const auto& instruction) {
            const auto* comparison = llvm::dyn_cast<llvm::ICmpInst>(&instruction);
            if (comparison == nullptr || comparison->getPredicate() != llvm::CmpInst::ICMP_ULT) {
              return false;
            }
            const auto* length = llvm::dyn_cast<llvm::ZExtInst>(comparison->getOperand(1));
            return length != nullptr &&
                   length->getSrcTy()->isIntegerTy(std::numeric_limits<std::uint32_t>::digits);
          });
        };
        EXPECT_EQ(bounds_checks(generic), 1);
        EXPECT_EQ(bounds_checks(specialized), 0);
        const auto null_checks = [](const llvm::Function& function) {
          return std::ranges::count_if(llvm::instructions(function), [](const auto& instruction) {
            const auto* comparison = llvm::dyn_cast<llvm::ICmpInst>(&instruction);
            return comparison != nullptr && comparison->getPredicate() == llvm::CmpInst::ICMP_NE &&
                   comparison->getOperand(0)->getType()->isPointerTy() &&
                   llvm::isa<llvm::ConstantPointerNull>(comparison->getOperand(1));
          });
        };
        EXPECT_GT(null_checks(generic), null_checks(specialized));
      });
}

TEST(NativeCompilerTest, EmitsConcreteGenericMethodsAndInterfaceSelectors) {
  for (const auto source : {
           ::vela::compiler::test::kGenericMethodsSource,
           ::vela::compiler::test::kMapLiteralSource,
           ::vela::compiler::test::kMapLiteralAsyncSource,
           ::vela::compiler::test::kWrappedGenericMethodValuesSource,
       }) {
    const auto prepared = Prepare(source);
    ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
    const auto result = Compile(prepared.program, {.exports = {"main"}, .preserve_ir = true});
    ASSERT_TRUE(result) << frontend::DiagnosticsText(result.error());
    EXPECT_FALSE(result->object.empty());
    EXPECT_EQ(result->exports.size(), 1U);
  }
}

TEST(NativeCompilerTest, ExceptionRootCleanupGeneratesLinearCodeSize) {
  constexpr std::array<std::size_t, 2> kCallCounts{16, 64};
  std::array<std::size_t, 2> sizes{};
  for (std::size_t sample = 0; sample < kCallCounts.size(); ++sample) {
    std::string source = R"(
class Box { value: int; }
func identity(value: Box): Box { return value; }
export func main(): int { let total = 0; try {
)";
    for (std::size_t call = 0; call < kCallCounts.at(sample); ++call) {
      source += "total += identity(Box(value: 1)).value;";
    }
    source += "} catch (error: Error) { return -1; } return total; }";
    const auto prepared = Prepare(source);
    ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
    const auto calls = native::PlanNativeCalls(prepared.program, NativeCodeTier::Baseline);
    const auto fields = native::FindFieldTargets(prepared.program);
    const auto generated = native::Generate(
        prepared.program, calls, fields, "root_cleanup", native::NativeDebugging::Safepoints,
        NativeCodeTier::Baseline, native::EscapeOptimization::Disabled);
    ASSERT_TRUE(generated) << frontend::DiagnosticsText(generated.error());
    for (const auto& function : *generated->module) {
      sizes.at(sample) += function.getInstructionCount();
    }
  }
  // 调用数扩大四倍时，异常根清理不能随失败点数与根槽数的乘积增长。
  ASSERT_GT(sizes.front(), 0U);
  EXPECT_LT(sizes.back(), sizes.front() * 5);
}

TEST(NativeCompilerTest, EmitsAsyncGenericMethodsWithConcreteExceptionLayouts) {
  const auto prepared = Prepare(::vela::compiler::test::kGenericAsyncMethodsSource);
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto result = Compile(prepared.program, {.exports = {"main"}});
  ASSERT_TRUE(result) << frontend::DiagnosticsText(result.error());
  EXPECT_FALSE(result->object.empty());
}

TEST(NativeCompilerTest, VerifiesBooleanNegationAndEmptyVoidBodies) {
  const auto prepared = Prepare(R"(
export func empty(): void {}
export func negate(value: bool): bool { return !value; }
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"empty", "negate"}});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  EXPECT_FALSE(result->object.empty());
  EXPECT_TRUE(result->llvm_ir.empty());
}

TEST(NativeCompilerTest, DenseIntegerSwitchAndMatchUseConstantTableDispatch) {
  const auto prepared = Prepare(R"(
export func choose(value: int): int {
  switch (value) {
    case 0: return 13; case 1: return 37; case 2: return 9; case 3: return 81;
    case 4: return 16; case 5: return 62; case 6: return 11; case 7: return 25;
    default: return 0;
  }
}
export func select(value: int): int {
  return match (value) {
    0 => 13, 1 => 37, 2 => 9, 3 => 81, 4 => 16, 5 => 62, 6 => 11, 7 => 25, _ => 0,
  };
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result =
      Compile(prepared.program, {.exports = {"choose", "select"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  llvm::LLVMContext context;
  const auto module = ParseModule(result->llvm_ir, context);
  ASSERT_NE(module, nullptr);
  for (const auto* name : {"vela_aot_entry_0", "vela_aot_entry_1"}) {
    const auto* function = module->getFunction(name);
    ASSERT_NE(function, nullptr);
    // LLVM 可合并两种语法生成的相同常量表；检查实际查表加载，不绑定私有表名。
    EXPECT_TRUE(std::ranges::any_of(llvm::instructions(*function), [](const auto& instruction) {
      const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (load == nullptr) {
        return false;
      }
      const auto* element = llvm::dyn_cast<llvm::GetElementPtrInst>(load->getPointerOperand());
      if (element == nullptr) {
        return false;
      }
      const auto* table = llvm::dyn_cast<llvm::GlobalVariable>(element->getPointerOperand());
      if (table == nullptr || !table->isConstant() || !table->hasInitializer()) {
        return false;
      }
      const auto* values = llvm::dyn_cast<llvm::ConstantDataArray>(table->getInitializer());
      constexpr unsigned kCaseCount = 8;
      return values != nullptr && values->getNumElements() == kCaseCount;
    }));
  }
}

TEST(NativeCompilerTest, ReferenceLeafFunctionsRegisterRootsOnlyAtFailure) {
  const auto prepared = Prepare(R"(
export func read(values: int[], index: int): int { return values[index]; }
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"read"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  const auto& text = result->llvm_ir;
  EXPECT_EQ(text.find("@vela_native_v1_frame_storage"), std::string::npos);
  EXPECT_EQ(text.find("@vela_native_v1_frame_release"), std::string::npos);
  EXPECT_NE(text.find("@vela_native_v2_leaf_failure"), std::string::npos);
  EXPECT_EQ(text.find("source.position"), std::string::npos);
}

std::vector<const llvm::Value*> EscapedRuntimeResults(const llvm::Function& function) {
  std::vector<const llvm::Value*> escaped;
  for (const auto& instruction : llvm::instructions(function)) {
    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
    if (call != nullptr && call->isIndirectCall() &&
        call->getType()->isIntegerTy(std::numeric_limits<std::uint8_t>::digits)) {
      const auto* output = call->getArgOperand(call->arg_size() - 1);
      escaped.push_back(llvm::getUnderlyingObject(output));
    }
  }
  return escaped;
}

TEST(NativeCompilerTest, KeepsInlinedScalarResultsOutOfEscapedRuntimeScratch) {
  const auto prepared = Prepare(R"(
external func text(): string;
func mix(value: int): int { return (value ^ 42) & 65535; }
export func run(values: int[]): int {
  const retained = text();
  let result = 0;
  for (let index = 0; index < 64; index++) {
    result ^= mix(values[index]) ^ mix(index);
  }
  return result + retained.length;
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"run"}, .preserve_ir = true});
  ASSERT_TRUE(result) << result.error().front().message;
  llvm::LLVMContext context;
  const auto module = ParseModule(result->llvm_ir, context);
  ASSERT_NE(module, nullptr);
  std::size_t checked_calls = 0;
  std::size_t loops_seen = 0;
  WithFunctionLoops(*module, [&](llvm::Function& function, const llvm::LoopInfo& loops) {
    const auto escaped = EscapedRuntimeResults(function);
    checked_calls += escaped.size();
    for (const auto* loop : loops.getLoopsInPreorder()) {
      ++loops_seen;
      for (const auto* block : loop->blocks()) {
        for (const auto& instruction : *block) {
          const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
          if (store != nullptr && store->getValueOperand()->getType()->isIntegerTy(
                                      std::numeric_limits<std::int64_t>::digits + 1)) {
            const auto* owner = llvm::getUnderlyingObject(store->getPointerOperand());
            EXPECT_EQ(std::ranges::find(escaped, owner), escaped.end()) << result->llvm_ir;
          }
        }
      }
    }
  });
  EXPECT_GT(checked_calls, 0U);
  EXPECT_GT(loops_seen, 0U);
}

struct LoopOperations final {
  bool has_array_load = false;
  bool has_overflow_check = false;
  bool has_rotation = false;
  bool observable = false;
};

LoopOperations AnalyzeLoopOperations(const llvm::Loop& loop) {
  LoopOperations operations;
  for (const auto* block : loop.blocks()) {
    for (const auto& instruction : *block) {
      if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
        operations.observable |= load->isAtomic();
        operations.has_array_load |=
            load->getType()->isIntegerTy(std::numeric_limits<std::uint64_t>::digits);
      }
      if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
        const auto* callee = call->getCalledFunction();
        operations.observable |= callee == nullptr || !callee->isIntrinsic();
        if (callee != nullptr) {
          const auto name = callee->getName();
          operations.has_overflow_check |= name.starts_with("llvm.sadd.with.overflow");
          operations.has_rotation |= name.starts_with("llvm.fsh");
        }
      }
    }
  }
  return operations;
}

TEST(NativeCompilerTest, BoundsPureInlinedCallsInArrayLoops) {
  const auto prepared = Prepare(R"(
func step(value: int): int { return value + 1; }
export func run(values: int[]): int {
  const count = values.length;
  let result = 0;
  for (let index = 0; index < count; index++) { result += step(values[index]); }
  return result;
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"run"}, .preserve_ir = true});
  ASSERT_TRUE(result) << frontend::DiagnosticsText(result.error());
  llvm::LLVMContext context;
  const auto module = ParseModule(result->llvm_ir, context);
  ASSERT_NE(module, nullptr);
  std::size_t bounded_loops = 0;
  WithFunctionLoops(*module, [&](llvm::Function&, const llvm::LoopInfo& loops) {
    for (const auto* loop : loops.getLoopsInPreorder()) {
      if (!loop->getSubLoops().empty()) {
        continue;
      }
      const auto operations = AnalyzeLoopOperations(*loop);
      if (operations.has_array_load && operations.has_overflow_check && !operations.observable) {
        ++bounded_loops;
      }
    }
  });
  EXPECT_GT(bounded_loops, 0U) << result->llvm_ir;
}

TEST(NativeCompilerTest, UsesWordRotationsForProvenLoopValues) {
  const auto prepared = Prepare(R"(
func rotate(value: int, bits: int): int {
  return ((value >>> bits) | (value << (32 - bits))) & 4294967295;
}
export func iterate(seed: int, count: int): int {
  let value = seed & 4294967295;
  for (let i = 0; i < count; i++) {
    value = (rotate(value, 7) + 42) & 4294967295;
  }
  return value;
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"iterate"}, .preserve_ir = true});
  ASSERT_TRUE(result) << result.error().front().message;
  EXPECT_TRUE(result->llvm_ir.contains("@llvm.fshr.i32") ||
              result->llvm_ir.contains("@llvm.fshl.i32"))
      << result->llvm_ir;
}

TEST(NativeCompilerTest, VersionsLazyGlobalGuardsBeforeBoundingInlinedScalarCalls) {
  const auto prepared = Prepare(R"(
const terms = [3, 5, 7, 11];
func rotate(value: int): int { return ((value >>> 7) | (value << 25)) & 4294967295; }
export func calculate(seed: int): int {
  let value = seed & 4294967295;
  for (let block = 0; block < 3; block++) {
    for (let index = 0; index < 64; index++) {
      value = (rotate(value) + terms[index & 3]) & 4294967295;
    }
  }
  return value;
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto result = Compile(prepared.program, {.exports = {"calculate"}, .preserve_ir = true});
  ASSERT_TRUE(result) << frontend::DiagnosticsText(result.error());
  llvm::LLVMContext context;
  const auto module = ParseModule(result->llvm_ir, context);
  ASSERT_NE(module, nullptr);
  std::size_t clean_loops = 0;
  WithFunctionLoops(*module, [&](llvm::Function&, const llvm::LoopInfo& loops) {
    for (const auto* loop : loops.getLoopsInPreorder()) {
      const auto operations = AnalyzeLoopOperations(*loop);
      clean_loops += static_cast<std::size_t>(operations.has_rotation && !operations.observable);
    }
  });
  EXPECT_GT(clean_loops, 0U) << result->llvm_ir;
}

TEST(NativeCompilerTest, VectorizesIndependentObjectFieldUpdates) {
  const auto prepared = Prepare(R"(
export class Particle { x: double; y: double; z: double; vx: double; vy: double; vz: double; }
export func update(points: Particle[]): void {
  for (const point in points) {
    point.x += point.vx;
    point.y += point.vy;
    point.z += point.vz;
  }
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"update"}, .preserve_ir = true});
  ASSERT_TRUE(result) << result.error().front().message;
  EXPECT_TRUE(result->llvm_ir.contains("fadd <2 x double>")) << result->llvm_ir;
}

struct LoopHostCalls {
  std::vector<const llvm::CallBase*> direct;
  std::vector<const llvm::StoreInst*> frame_stores;
  bool has_checked = false;
};

TEST(NativeCompilerTest, ExecutionFrameAndDepthStoresShareAliasMetadataAcrossHostCalls) {
  const auto prepared = Prepare(R"(
external func step(value: int): int;
export func calculate(count: int): int {
  let result = 0;
  for (let index = 0; index < count; index++) { result = step(result); }
  return result;
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto generated = GenerateForTest(prepared.program, "frames", NativeCodePlacement::Linked);
  ASSERT_TRUE(generated) << frontend::DiagnosticsText(generated.error());
  const auto& module = *generated->module;
  std::size_t frame_stores = 0;
  std::size_t depth_stores = 0;
  for (const auto& function : module) {
    if (function.isDeclaration() || function.arg_empty()) {
      continue;
    }
    for (const auto& instruction : llvm::instructions(function)) {
      const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
      if (store == nullptr) {
        continue;
      }
      std::int64_t offset = 0;
      const auto* base = llvm::GetPointerBaseWithConstantOffset(store->getPointerOperand(), offset,
                                                                module.getDataLayout());
      if (base != function.getArg(0)) {
        continue;
      }
      using runtime::NativeMemoryLayout;
      if (std::cmp_equal(offset, NativeMemoryLayout::kExecutionFrame)) {
        ++frame_stores;
      } else if (std::cmp_equal(offset, NativeMemoryLayout::kExecutionDepth)) {
        ++depth_stores;
      } else {
        continue;
      }
      EXPECT_NE(store->getMetadata(llvm::LLVMContext::MD_tbaa), nullptr)
          << function.getName().str();
    }
  }
  EXPECT_GE(frame_stores, 4U);
  EXPECT_EQ(frame_stores, depth_stores);
}

LoopHostCalls CollectHostCalls(const llvm::Loop& loop) {
  LoopHostCalls calls;
  for (const auto* block : loop.blocks()) {
    for (const auto& instruction : *block) {
      const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      if (call != nullptr && call->isIndirectCall()) {
        if (call->getType()->isIntegerTy(std::numeric_limits<std::int64_t>::digits + 1)) {
          calls.direct.push_back(call);
        } else {
          calls.has_checked = true;
        }
      }
      const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
      if (store != nullptr && llvm::getUnderlyingObject(store->getPointerOperand())
                                  ->getName()
                                  .starts_with("import.frame")) {
        calls.frame_stores.push_back(store);
      }
    }
  }
  return calls;
}

TEST(NativeCompilerTest, SelectsDirectOrCheckedHostAbiOutsideTheLoop) {
  const auto prepared = Prepare(R"(
external func step(value: int): int;
export func calculate(count: int): int {
  let result = 0;
  for (let index = 0; index < count; index++) { result = step(result); }
  return result;
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"calculate"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  llvm::LLVMContext context;
  const auto module = ParseModule(result->llvm_ir, context);
  ASSERT_NE(module, nullptr);
  std::size_t direct_loops = 0;
  WithFunctionLoops(*module, [&](llvm::Function&, const llvm::LoopInfo& loops) {
    for (const auto* loop : loops.getLoopsInPreorder()) {
      const auto calls = CollectHostCalls(*loop);
      if (calls.direct.empty()) {
        continue;
      }
      ++direct_loops;
      EXPECT_FALSE(calls.has_checked) << result->llvm_ir;
      for (const auto* call : calls.direct) {
        const auto argument_count = call->arg_size();
        EXPECT_TRUE(argument_count == 1U || argument_count == 2U);
        for (const auto* user : call->users()) {
          if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user)) {
            EXPECT_FALSE(loop->contains(store->getParent())) << result->llvm_ir;
          }
        }
      }
      for (const auto* store : calls.frame_stores) {
        // 循环内只重置函数与指令位置；优化器可将相邻的整数常量写入合成向量。
        const auto* value = store->getValueOperand();
        EXPECT_TRUE(llvm::isa<llvm::Constant>(value)) << result->llvm_ir;
        EXPECT_TRUE(value->getType()->isIntOrIntVectorTy()) << result->llvm_ir;
      }
      const auto frame_store_count = calls.frame_stores.size();
      EXPECT_GE(frame_store_count, 1U) << result->llvm_ir;
      EXPECT_LE(frame_store_count, 2U) << result->llvm_ir;
    }
  });
  EXPECT_GT(direct_loops, 0U) << result->llvm_ir;
}

TEST(NativeCompilerTest, BorrowsStringConstantsDirectlyInsideHostCallLoops) {
  const auto prepared = Prepare(R"(
external func length(text: string): int;
export func calculate(count: int): int {
  let result = 0;
  for (let index = 0; index < count; index++) { result += length('borrowed'); }
  return result;
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"calculate"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  llvm::LLVMContext context;
  const auto module = ParseModule(result->llvm_ir, context);
  ASSERT_NE(module, nullptr);
  EXPECT_EQ(module->getFunction("vela_native_v1_constant"), nullptr);
  std::size_t direct_loops = 0;
  WithFunctionLoops(*module, [&](llvm::Function&, const llvm::LoopInfo& loops) {
    for (const auto* loop : loops.getLoopsInPreorder()) {
      const auto calls = CollectHostCalls(*loop);
      if (calls.direct.empty()) {
        continue;
      }
      ++direct_loops;
      EXPECT_FALSE(calls.has_checked);
      for (const auto* call : calls.direct) {
        ASSERT_EQ(call->arg_size(), 2U);
        const auto* argument = llvm::dyn_cast<llvm::Instruction>(call->getArgOperand(1));
        ASSERT_NE(argument, nullptr);
        EXPECT_FALSE(loop->contains(argument->getParent())) << result->llvm_ir;
      }
    }
  });
  EXPECT_GT(direct_loops, 0U) << result->llvm_ir;
}

TEST(NativeCompilerTest, NullComparisonsUsePresenceForReferencesScalarsAndDynamicValues) {
  const auto prepared = Prepare(R"(
export class Box { value: int = 0; }
export func object(value: Box?): bool { return value == null; }
export func text(value: string?): bool { return null == value; }
export func integer(value: int?): bool { return value != null; }
export func erased(value: dynamic): bool { return value == null; }
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(
      prepared.program, {.exports = {"object", "text", "integer", "erased"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  EXPECT_EQ(result->llvm_ir.find("@vela_native_v1_equal"), std::string::npos);
}

TEST(NativeCompilerTest, KnownLocalClosuresCallTheirTypedBodiesDirectlyInsideSort) {
  const auto prepared = Prepare(R"(
export func order(values: int[], direction: int): void {
  values.sort((left, right): int => left > right ? direction : (left < right ? -direction : 0));
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"order"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  EXPECT_EQ(result->llvm_ir.find("@vela_native_v1_call_indirect"), std::string::npos);
  bool direct_comparator = false;
  for (const auto& function : result->program.functions) {
    for (const auto& instruction : function.instructions) {
      if (instruction.opcode == Opcode::Call) {
        const auto& target = result->program.functions.at(instruction.index);
        direct_comparator = direct_comparator || target.parameter_count == 3;
      }
    }
  }
  EXPECT_TRUE(direct_comparator);
}

TEST(NativeCompilerTest, NumericConversionsUseTypedInstructionsWithoutRuntimeDispatch) {
  const auto prepared = Prepare(R"(
export func widen(value: int): double { return value.toDouble(); }
export func truncate(value: double): int { return value.toInt(); }
export func integer(value: int): int { return value.toInt(); }
export func decimal(value: double): double { return value.toDouble(); }
export func sum(values: double[]): double {
  let sum = 0.0;
  for (let index = 0; index < 500; index++) { sum += values[index] / (index + 1).toDouble(); }
  return sum;
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result =
      Compile(prepared.program,
              {.exports = {"widen", "truncate", "integer", "decimal", "sum"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  const auto& text = result->llvm_ir;
  EXPECT_EQ(text.find("@vela_native_v1_builtin"), std::string::npos);
  EXPECT_EQ(text.find("@llvm.sadd.with.overflow"), std::string::npos);
  EXPECT_NE(text.find("sitofp i64"), std::string::npos);
  EXPECT_NE(text.find("fptosi double"), std::string::npos);
  EXPECT_NE(text.find("@vela_native_v2_leaf_failure"), std::string::npos);
  llvm::LLVMContext context;
  const auto module = ParseModule(text, context);
  ASSERT_NE(module, nullptr);
  std::size_t loop_blocks = 0;
  WithFunctionLoops(*module, [&](llvm::Function&, const llvm::LoopInfo& loops) {
    for (auto* loop : loops.getLoopsInPreorder()) {
      for (const auto* block : loop->blocks()) {
        ++loop_blocks;
        for (const auto& instruction : *block) {
          if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
            const auto* callee = call->getCalledFunction();
            EXPECT_TRUE(callee != nullptr && callee->isIntrinsic()) << text;
          }
          EXPECT_FALSE(llvm::isa<llvm::StoreInst>(instruction)) << text;
        }
      }
    }
  });
  EXPECT_GT(loop_blocks, 0U);
}

TEST(NativeCompilerTest, NumericParsingUsesTheSharedParserWithoutGenericBuiltinDispatch) {
  const auto prepared = Prepare(R"(
export func integer(text: string, radix: int?): int { return int.parse(text, radix: radix); }
export func tryInteger(text: string): int? { return int.tryParse(text); }
export func decimal(text: string): double { return double.parse(text); }
export func tryDecimal(text: string): double? { return double.tryParse(text); }
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto result =
      Compile(prepared.program,
              {.exports = {"integer", "tryInteger", "decimal", "tryDecimal"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << frontend::DiagnosticsText(result.error());
  const auto& text = result->llvm_ir;
  EXPECT_EQ(text.find("@vela_native_v1_builtin"), std::string::npos);
  EXPECT_NE(text.find("@vela_native_v1_numeric_parse"), std::string::npos);
}

TEST(NativeCompilerTest, ReadsReadyGlobalsDirectlyAndInitializesOnlyOnTheOtherBranch) {
  const auto prepared = Prepare(R"(
let count = 42;
export func read(): int { return count; }
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result =
      Compile(prepared.program,
              {.optimization = NativeOptimization::None, .exports = {"read"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  llvm::LLVMContext context;
  const auto module = ParseModule(result->llvm_ir, context);
  ASSERT_NE(module, nullptr);
  ASSERT_EQ(result->exports.size(), 1U);
  const auto* body =
      module->getFunction("vela_aot_body_" + std::to_string(result->exports.front().function));
  ASSERT_NE(body, nullptr);
  std::size_t ready_blocks = 0;
  std::size_t initializer_calls = 0;
  for (const auto& block : *body) {
    if (block.getName() == "global.ready") {
      ++ready_blocks;
      for (const auto& instruction : block) {
        EXPECT_FALSE(llvm::isa<llvm::CallBase>(instruction));
      }
    }
    for (const auto& instruction : block) {
      const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      if (call == nullptr) {
        continue;
      }
      const auto* callee = call->getCalledFunction();
      if (callee != nullptr && callee->getName() == "vela_native_v2_initialize_global") {
        ++initializer_calls;
        EXPECT_EQ(block.getName(), "global.initialize");
      }
    }
  }
  EXPECT_EQ(ready_blocks, 1U);
  EXPECT_EQ(initializer_calls, 1U);
}

TEST(NativeCompilerTest, EliminatesIntegerReductionsWithDynamicBounds) {
  const auto sources = std::to_array<std::string_view>({
      "let sum = initial; for (let index = 0; index < bound; index++) { sum += index; }",
      "let sum = initial; for (let index = 0; index > bound; index--) { sum += index; }",
      "let sum = initial; for (let index = 0; index < bound; index++) { sum -= index; }",
      "let sum = initial; for (let index = 0; index < bound; index++) { sum += term; }",
      "let sum = initial; for (let index = 0; index < bound; index++) { sum -= term; }",
      "let sum = initial; for (let index = start; index < bound; index++) { sum += term; }",
      "let sum = initial; for (let index = start; index < 0; index++) { sum += index; }",
      "let sum = initial; for (let index = start; index > 0; index--) { sum += index; }",
      "let sum = initial; for (let index = 0; bound > index; index++) { sum += index; }",
      "let sum = initial; for (let index = start; index < bound; index++) { sum += index; }",
      "let sum = initial; for (let index = start; index > bound; index--) { sum += index; }",
      "let sum = initial; for (let index = start; index < bound; index++) { sum -= index; }",
      "let sum = initial; for (let index = start; index > bound; index--) { sum -= index; }",
  });
  for (const auto source : sources) {
    SCOPED_TRACE(source);
    const auto prepared =
        Prepare("export func sum(bound: int, initial: int, term: int, start: int): int { " +
                std::string(source) + " return sum; }");
    ASSERT_TRUE(prepared.Succeeded());
    const auto result = Compile(prepared.program, {.exports = {"sum"}, .preserve_ir = true});
    ASSERT_TRUE(result.has_value()) << result.error().front().message;
    llvm::LLVMContext context;
    const auto module = ParseModule(result->llvm_ir, context);
    ASSERT_NE(module, nullptr);
    WithFunctionLoops(*module, [&](llvm::Function& function, const llvm::LoopInfo& loops) {
      EXPECT_TRUE(loops.empty()) << function.getName().str() << '\n' << result->llvm_ir;
    });
    EXPECT_EQ(result->llvm_ir.find("@llvm.sadd.with.overflow"), std::string::npos);
    EXPECT_EQ(result->llvm_ir.find("@llvm.ssub.with.overflow"), std::string::npos);
    EXPECT_NE(result->llvm_ir.find("@vela_native_v2_leaf_failure"), std::string::npos);
  }
}

TEST(NativeCompilerTest, VectorizesBoundedClosedGlobalArrayReductionsAfterInlining) {
  const auto prepared = Prepare(R"(
let values: int[] = [];
export func prepare(count: int, seed: int): void {
  values.clear();
  values.reserve(count);
  for (let index = 0; index < count; index++) { values.push((seed ^ index) & 1023); }
}
func increment(value: int): int { return value + 1; }
export func sum(): int {
  const data = values;
  const count = data.length;
  let total = 0;
  for (let index = 0; index < count; index++) { total += increment(data[index]); }
  return total;
}
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto result =
      Compile(prepared.program, {.exports = {"prepare", "sum"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << frontend::DiagnosticsText(result.error());
  llvm::LLVMContext context;
  const auto module = ParseModule(result->llvm_ir, context);
  ASSERT_NE(module, nullptr);
  const auto entry = std::ranges::find(result->exports, "sum", &NativeExport::name);
  ASSERT_NE(entry, result->exports.end());
  const auto* function = module->getFunction("vela_aot_body_" + std::to_string(entry->function));
  ASSERT_NE(function, nullptr);
  const auto vector_sum =
      std::ranges::any_of(llvm::instructions(function), [](const auto& instruction) {
        const auto* type = instruction.getType();
        return instruction.getOpcode() == llvm::Instruction::Add && type->isVectorTy() &&
               type->getScalarType()->isIntegerTy(std::numeric_limits<std::uint64_t>::digits);
      });
  EXPECT_TRUE(vector_sum) << result->llvm_ir;
}

TEST(NativeCompilerTest, FoldsUnobservedUnitFieldUpdates) {
  const auto prepared = Prepare(R"(
class Box { value: int; }
export func fields(bound: int): int {
  const box = Box(value: 0);
  for (let index = 0; index < bound; index++) { box.value += 1; }
  return box.value;
}
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"fields"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  llvm::LLVMContext context;
  const auto module = ParseModule(result->llvm_ir, context);
  ASSERT_NE(module, nullptr);
  WithFunctionLoops(*module, [&](llvm::Function&, const llvm::LoopInfo& loops) {
    EXPECT_TRUE(loops.empty()) << result->llvm_ir;
  });
}

TEST(NativeCompilerTest, PreservesArrayStorageWhenTemporaryValuesAreErasedToDynamic) {
  const auto prepared = Prepare(R"(
export func value(): dynamic { return [1, 2, 3] as dynamic; }
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"value"}});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  EXPECT_FALSE(result->object.empty());
}

TEST(NativeCompilerTest, ArrayAppendZeroExtendsLengthsBeforeAddressingElements) {
  const auto prepared = Prepare(R"(
export func append(values: int[], value: int): int { return values.push(value); }
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(
      prepared.program,
      {.optimization = NativeOptimization::None, .exports = {"append"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  // uint32 长度的最高位不是符号位；i32 GEP 会将 2 Gi 元素以上的位置符号扩展。
  const auto& text = result->llvm_ir;
  const auto address = text.find("array.element = getelementptr i64,");
  ASSERT_NE(address, std::string::npos);
  const auto line = std::string_view(text).substr(address, text.find('\n', address) - address);
  EXPECT_NE(line.find(", i64 "), std::string_view::npos);
  EXPECT_EQ(line.find(", i32 "), std::string_view::npos);
}

TEST(NativeCompilerTest, ExportsOnlyPublicFunctionsFromTheEntryModule) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = "import 'library'; func hidden(): int { return 1; }"},
      {.path = "library.vela", .text = "export func imported(): int { return 2; }"},
  });
  const auto prepared = ir::PrepareParsed(frontend::LoadSources(sources, "main.vela"));
  ASSERT_TRUE(prepared.Succeeded());
  const auto private_export = Compile(prepared.program, {.exports = {"hidden"}});
  ASSERT_FALSE(private_export.has_value());
  EXPECT_NE(private_export.error().front().message.find("public entry-module"), std::string::npos);
  const auto imported_export = Compile(prepared.program, {.exports = {"imported"}});
  ASSERT_FALSE(imported_export.has_value());
}

TEST(NativeCompilerTest, RejectsModuleNamesThatCannotProduceValidCHeaders) {
  const auto prepared = Prepare("export func value(): int { return 42; }");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result =
      Compile(prepared.program, {.module_name = "invalid-module", .exports = {"value"}});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().front().message.find("C identifier"), std::string::npos);
}

TEST(NativeCompilerTest, EmitsPrivateAsyncMainAndRejectsCustomHostImports) {
  const auto prepared = Prepare("async func main(): Future<int> { return await 42; }");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result =
      Compile(prepared.program,
              {.exports = {}, .entry_mode = NativeEntryMode::Executable, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  EXPECT_NE(result->llvm_ir.find("define i32 @main()"), std::string::npos);
  EXPECT_NE(result->llvm_ir.find("@vela_native_v1_main"), std::string::npos);
  EXPECT_TRUE(result->exports.empty());

  const auto custom = Prepare("external func host(): int; func main(): int { return host(); }");
  ASSERT_TRUE(custom.Succeeded());
  const auto rejected =
      Compile(custom.program, {.exports = {}, .entry_mode = NativeEntryMode::Executable});
  ASSERT_FALSE(rejected.has_value());
  EXPECT_NE(rejected.error().front().message.find("custom host binding"), std::string::npos);
  EXPECT_TRUE(Compile(custom.program).has_value());
}

TEST(NativeCompilerTest, RemovesUnusedImportsAndRetainsOnlySelectedEntryCode) {
  const auto prepared = Prepare(R"(
external func unused(): int;
func removed(): int { return unused(); }
export func answer(): int { return 42; }
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto result = Compile(prepared.program, {.exports = {"answer"}, .preserve_ir = true});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  ASSERT_EQ(result->program.functions.size(), 1U);
  EXPECT_EQ(result->program.functions.front().name, "answer");
  EXPECT_EQ(result->exports.front().function, 0U);
  llvm::LLVMContext context;
  const auto module = ParseModule(result->llvm_ir, context);
  ASSERT_NE(module, nullptr);
  EXPECT_EQ(module->getFunction("vela_native_v1_import"), nullptr);
}

}  // namespace
}  // namespace vela::compiler::aot
