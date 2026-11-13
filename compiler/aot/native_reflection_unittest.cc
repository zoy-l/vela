#include "compiler/aot/native_compiler.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include <llvm/ADT/StringRef.h>
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/CoreContainers.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/TargetParser/Triple.h>

#include "base/execution_status.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/engine.h"
#include "compiler/test_support/preparation_test_support.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_api.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/value.h"

namespace vela::compiler::aot {
namespace {

constexpr std::string_view kMutableArguments = R"(
[Reflect]
class Counter {
  value: int = 0;
  calls: int = 0;
  update(value: int): int { this.calls++; this.value = value; return value + 1; }
}
const info = Reflect.typeInfo<Counter>();
const cached = info.findMethod('update');
const argument = CallArgument(value: 0);
const arguments = CallArguments(items: [argument]);
export func exercise(): int {
  const found = cached;
  if (found == null) { return -1; }
  const method: MethodInfo<Counter> = found;
  const object = Counter();
  const retained = '${object.value}';
  for (let value = 0; value < 4; value++) {
    argument.value = value;
    if (method.invoke(object, arguments) != value + 1) { return -2; }
  }
  argument.value = 'wrong';
  try { method.invoke(object, arguments); return -3; }
  catch (error: Error) { if (error.code != 'InvalidCast') { return -4; } }
  if (object.calls != 4 || object.value != 3) { return -5; }
  argument.value = 7;
  argument.name = 'value';
  if (method.invoke(object, arguments) != 8) { return -6; }
  argument.name = 'missing';
  try { method.invoke(object, arguments); return -7; }
  catch (error: Error) { if (error.code != 'TypeError') { return -8; } }
  argument.name = null;
  arguments.items = [];
  try { method.invoke(object, arguments); return -9; }
  catch (error: Error) { if (error.code != 'TypeError') { return -10; } }
  arguments.items = [argument, argument];
  try { method.invoke(object, arguments); return -11; }
  catch (error: Error) { if (error.code != 'TypeError') { return -12; } }
  if (object.calls != 5 || object.value != 7) { return -13; }
  arguments.items = [argument];
  argument.value = 9223372036854775807;
  try { method.invoke(object, arguments); return -14; }
  catch (error: Error) { if (error.code != 'IntegerOverflow') { return -15; } }
  return object.calls == 6 && object.value == 9223372036854775807 && retained == '0' ? 42 : -16;
}
export func fail(): int {
  const method = cached;
  if (method == null) { return 0; }
  method.invoke(Counter(), CallArguments(items: [CallArgument(value: 9223372036854775807)]));
  return 0;
}
export func lookup(): int {
  const object = Counter();
  if (info.findMethod('missing') != null) { return -1; }
  let sum = 0;
  for (let value = 0; value < 2051; value++) {
    const method = info.findMethod('update');
    if (method == null) { return -2; }
    const result = method.invoke(object, CallArguments(items: [CallArgument(value: value)]));
    if (!(result is int)) { return -3; }
    sum += result;
  }
  if (object.calls != 2051 || object.value != 2050 || sum != 2104326) { return -4; }
  for (let value = 0; value < 10; value++) {
    const method = info.findMethod('update');
    if (method == null) { return -5; }
    if (method.invoke(object, CallArguments(items: [CallArgument(value: value)])) != value + 1) {
      return -6;
    }
    if (info.findMethod('missing') != null) { return -7; }
  }
  return object.calls == 2061 && object.value == 9 ? 42 : -8;
}
[Reflect]
class FieldValues {
  integer: int = 0;
  optional: int? = null;
  flag: bool = false;
  number: double = 0.0;
  text: string = '';
  anything: dynamic = null;
  nested: Counter = Counter();
}
class DerivedFieldValues extends FieldValues { tail: int = 99; }
export func fields(): int {
  const info = Reflect.typeInfo<FieldValues>();
  const foundInteger = info.findField('integer');
  const foundOptional = info.findField('optional');
  const foundFlag = info.findField('flag');
  const foundNumber = info.findField('number');
  const foundText = info.findField('text');
  const foundAnything = info.findField('anything');
  const foundNested = info.findField('nested');
  if (foundInteger == null || foundOptional == null || foundFlag == null || foundNumber == null ||
      foundText == null || foundAnything == null || foundNested == null) { return -1; }
  const integer: FieldInfo<FieldValues> = foundInteger;
  const optional: FieldInfo<FieldValues> = foundOptional;
  const flag: FieldInfo<FieldValues> = foundFlag;
  const number: FieldInfo<FieldValues> = foundNumber;
  const text: FieldInfo<FieldValues> = foundText;
  const anything: FieldInfo<FieldValues> = foundAnything;
  const nested: FieldInfo<FieldValues> = foundNested;
  const derived = DerivedFieldValues();
  const object: FieldValues = derived;
  for (let value = 0; value < 2051; value++) {
    integer.setValue(object, value);
    if (integer.getValue(object) != value) { return -2; }
  }
  try { integer.setValue(object, 'wrong'); return -3; }
  catch (error: Error) { if (error.code != 'InvalidCast') { return -4; } }
  if (object.integer != 2050 || derived.tail != 99) { return -5; }
  optional.setValue(object, 17);
  if (optional.getValue(object) != 17) { return -6; }
  optional.setValue(object, null);
  if (optional.getValue(object) != null) { return -7; }
  flag.setValue(object, true);
  number.setValue(object, 2.5);
  if (flag.getValue(object) != true || number.getValue(object) != 2.5) { return -8; }
  try { number.setValue(object, 2); return -9; }
  catch (error: Error) { if (error.code != 'InvalidCast') { return -10; } }
  text.setValue(object, 'value${object.integer}');
  anything.setValue(object, Counter(value: 23));
  nested.setValue(object, Counter(value: 29));
  let scratch = '';
  for (let index = 0; index < 9; index++) { scratch = 'scratch${index}'; }
  if (text.getValue(object) != 'value2050' || scratch != 'scratch8') { return -11; }
  const retained = anything.getValue(object);
  if (!(retained is Counter) || retained.value != 23 || object.nested.value != 29) { return -12; }
  try { text.setValue(object, null); return -13; }
  catch (error: Error) { if (error.code != 'InvalidCast') { return -14; } }
  try { nested.setValue(object, object); return -15; }
  catch (error: Error) { if (error.code != 'InvalidCast') { return -16; } }
  return object.text == 'value2050' && object.number == 2.5 && object.nested.value == 29 ? 42 : -17;
}
)";

TEST(NativeReflectionTest, OfflineMembersPreserveValueChecksAndSingleExecutionOnFailure) {
  const auto prepared = ir::test::Prepare(kMutableArguments);
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto artifact =
      Compile(prepared.program, {.exports = {"exercise", "fields", "fail", "lookup"}});
  ASSERT_TRUE(artifact) << frontend::DiagnosticsText(artifact.error());
  auto engine = jit::CreateEngine(llvm::Triple(artifact->target));
  ASSERT_TRUE(static_cast<bool>(engine)) << llvm::toString(engine.takeError());
  auto& loader = **engine;
  llvm::orc::SymbolMap symbols;
  symbols.try_emplace(loader.mangleAndIntern("vela_aot_v2_instantiate"),
                      llvm::orc::ExecutorAddr::fromPtr(&vela_aot_v2_instantiate),
                      llvm::JITSymbolFlags::Exported);
  symbols.try_emplace(loader.mangleAndIntern("vela_aot_v2_invoke"),
                      llvm::orc::ExecutorAddr::fromPtr(&vela_aot_v2_invoke),
                      llvm::JITSymbolFlags::Exported);
  auto definitions =
      loader.getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(symbols)));
  ASSERT_FALSE(static_cast<bool>(definitions)) << llvm::toString(std::move(definitions));
  const auto& object = artifact->object;
  const void* storage = object.data();
  const auto bytes = llvm::StringRef(static_cast<const char*>(storage), object.size());
  auto error = (*engine)->addObjectFile(llvm::MemoryBuffer::getMemBuffer(bytes, "", false));
  ASSERT_FALSE(static_cast<bool>(error)) << llvm::toString(std::move(error));
  auto symbol = (*engine)->lookup(artifact->image_symbol);
  ASSERT_TRUE(static_cast<bool>(symbol)) << llvm::toString(symbol.takeError());
  using ImageEntry = const runtime::NativeImage* (*)();
  const auto* image = symbol->toPtr<ImageEntry>()();
  runtime::Isolate isolate(1);
  const auto instance = runtime::NativeInstance::Instantiate(isolate, *image);
  ASSERT_TRUE(instance) << instance.error().message;
  for (const auto* name : std::array{"exercise", "fields", "lookup"}) {
    const auto function = (*instance)->FindFunction(name);
    ASSERT_TRUE(function) << name;
    const auto result = runtime::ExecuteNativeFunction(**instance, *function);
    ASSERT_TRUE(result.Succeeded()) << name << ": " << result.message;
    ASSERT_TRUE(result.value.IsInteger()) << name;
    EXPECT_EQ(result.value.AsInteger(), 42) << name;
  }
  isolate.heap().Collect();
  const auto fail = (*instance)->FindFunction("fail");
  ASSERT_TRUE(fail);
  const auto failure = runtime::ExecuteNativeFunction(**instance, *fail);
  EXPECT_EQ(failure.status, ExecutionStatus::IntegerOverflow);
  ASSERT_EQ(failure.stack_trace.size(), 2);
  EXPECT_NE(failure.stack_trace[0].function.find("update"), std::string::npos);
  EXPECT_EQ(failure.stack_trace[1].function, "fail");
}

}  // namespace
}  // namespace vela::compiler::aot
