"""Exercise source attributes and reflection through JIT and source-free native artifacts."""

from __future__ import annotations

import json
import os
from pathlib import Path
import sys
import unittest

from tools.aot_reflection_fixture import HOST as OFFLINE_MEMBER_HOST, SOURCE as OFFLINE_MEMBER_SOURCE
from tools.config import ROOT, executable_name
from tools.test_support import (CommandTestCase, compile_native_host, create_native_package,
                                native_command, native_test_libraries)


SOURCE = r'''
interface Reader { [Reflect] read(): int; }
[AttributeUsage(allowMultiple: true)]
class Button extends Reflect { label: string = ''; }
[Reflect]
class Counter implements Reader {
  value: int = 10;
  static total: int = 3;
  static get shared(): int { return Counter.total; }
  static set shared(value: int): void { Counter.total = value; }
  static twice(value: int): int { return value * 2; }
  [Reflect] private hidden: int = 9;
  get amount(): int { return this.value; }
  [Reflect] private set amount(value: int): void { this.value = value; }
  add(amount: int): int { this.value += amount; return this.value; }
  [Button(label: 'first')]
  [Button(label: 'second')]
  read(): int { return this.value; }
  echo<T>(value: T): T { return value; }
}
enum Choice { first, second, [Reflect] third }
class Generic<T> { value: T; }
func compiledSpecializations(): void {
  const counter = Counter();
  counter.echo<string>('compiled');
  counter.echo<(int) => string>((value: int): string => 'compiled');
}
func describe<T>(): TypeInfo<T> { return Reflect.typeInfo<T>(); }
export func calculate(): int {
  const info = describe<Counter>();
  if (info.name != 'Counter' || info.kind != TypeKind.Class) { return -1; }
  const ctor = info.getConstructor();
  if (ctor == null || !ctor.parameters[0].hasDefault) { return -2; }
  const created = ctor.invoke(CallArguments());
  if (!(created is Counter)) { return -20; }
  const object: Counter = created;
  const field = info.findField('value');
  if (field == null) { return -3; }
  field.setValue(object, 20);
  const property = info.findProperty('amount');
  if (property == null || !property.canRead || property.canWrite) { return -4; }
  try { property.setValue(object, 50); return -5; }
  catch (error: Error) { if (error.code != 'ReflectionAccessDenied') { return -6; } }
  const method = info.findMethod('add');
  if (method == null || method.parameters[0].name != 'amount') { return -7; }
  const answer = method.invoke(object, CallArguments(items: [CallArgument(value: 22, name: 'amount')]));
  if (!(answer is int)) { return -21; }
  const reader = Reflect.typeInfo<Reader>().findMethod('read');
  if (reader == null || reader.invoke(object, CallArguments()) != answer) { return -8; }
  const generic = info.findMethod('echo', typeArguments: [Reflect.typeInfo<string>()]);
  if (generic == null || generic.invoke(object, CallArguments(items: [CallArgument(value: 'ok')])) != 'ok') { return -9; }
  const selected = Reflect.typeInfo<Choice>().findEnumMember('third');
  if (selected == null || selected.ordinal != 2 || Reflect.typeOf(selected.value).id != Reflect.typeInfo<Choice>().id) { return -10; }
  const values: Counter[] = [];
  if (Reflect.typeOf(values).typeArguments[0].id != info.id) { return -11; }
  if (Reflect.voidType().kind != TypeKind.Void || Reflect.typeOf(null).kind != TypeKind.Null) { return -12; }
  const signature = Reflect.typeInfo<(int) => string>().signature;
  if (signature == null || signature.parameters[0].type.kind != TypeKind.Int || signature.returnType.kind != TypeKind.String) { return -13; }
  if (Reflect.typeInfo<Generic<(int) => string>>().typeArguments[0].id != Reflect.typeInfo<(int) => string>().id) { return -15; }
  const callable = info.findMethod('echo', typeArguments: [Reflect.typeInfo<(int) => string>()]);
  if (callable == null || callable.typeArguments[0].signature == null) { return -16; }
  const ids = Set<TypeId>();
  ids.add(info.id);
  if (!ids.has(Reflect.typeOf(object).id)) { return -14; }
  const matches = Reflect.findMethods<Button>();
  if (matches.length != 2 || matches[0].attribute.label != 'first' || matches[1].attribute.label != 'second') { return -17; }
  const read = matches[0].instanceMethod;
  if (read == null || matches[0].staticMethod != null || matches[0].method.name != 'read' || read.invoke(object, CallArguments()) != answer) { return -18; }
  matches[0].attribute.label = 'modified';
  if (Reflect.findMethods<Button>(module: info.module)[0].attribute.label != 'first') { return -19; }
  const total = info.findStaticField('total');
  const shared = info.findStaticProperty('shared');
  const twice = info.findStaticMethod('twice');
  if (total == null || shared == null || twice == null) { return -22; }
  total.setValue(21);
  if (shared.getValue() != 21) { return -23; }
  const getter = shared.getter;
  const setter = shared.setter;
  if (getter == null || setter == null) { return -24; }
  setter.invoke(CallArguments(items: [CallArgument(value: 42)]));
  if (getter.invoke(CallArguments()) != answer || total.getValue() != answer) { return -25; }
  if (twice.invoke(CallArguments(items: [CallArgument(value: 21)])) != answer) { return -26; }
  if (info.findField('total') != null || info.findStaticField('value') != null) { return -27; }
  return answer;
}
func main(): void { print(calculate()); }
'''

HOST = r'''
#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <vela/host.h>
#include "runtime/isolate/isolate.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_execution.h"
extern "C" const vela::runtime::NativeImage* IMAGE() noexcept;
TEST(ReflectionArtifactTest, QueriesAndInvokesUsingOnlyTheStandaloneRuntime) {
  vela::runtime::Isolate isolate(1);
  auto instance = vela::runtime::NativeInstance::Instantiate(isolate, *IMAGE());
  ASSERT_TRUE(instance) << instance.error().message;
  auto reflection = vela::Reflection::Bind(**instance);
  ASSERT_TRUE(reflection);
  const auto types = reflection->GetTypes(MODULE);
  ASSERT_TRUE(types);
  const auto info = reflection->FindType(MODULE, "Counter");
  ASSERT_TRUE(info && *info);
  const auto ctor = (*info)->GetConstructor();
  ASSERT_TRUE(ctor && *ctor);
  vela::HandleScope scope(isolate);
  const auto object = (*ctor)->Invoke(scope);
  ASSERT_TRUE(object) << object.error().message;
  const auto method = (*info)->FindMethod("add");
  ASSERT_TRUE(method && *method);
  const auto value = vela::CreateValue<std::int64_t>(scope, 32);
  ASSERT_TRUE(value);
  const std::array arguments{vela::CallArgument(*value, "amount")};
  const auto result = (*method)->Invoke(scope, *object, arguments);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(vela::internal::HandleAccess::Read(*result).AsInteger(), 42);
  const auto function = (*instance)->FindFunction("calculate");
  ASSERT_TRUE(function);
  const auto script = vela::runtime::ExecuteNativeFunction(**instance, *function);
  ASSERT_TRUE(script.Succeeded()) << script.message;
  EXPECT_EQ(script.value.AsInteger(), 42);
}
'''


@unittest.skipUnless(sys.platform == "win32" or os.environ.get("VELA_TEST_TARGET_SUPPORT_SPEC"),
                     "Native reflection acceptance requires an audited VELA_TEST_TARGET_SUPPORT_SPEC")
class ReflectionCommandTest(CommandTestCase):
    def setUp(self) -> None:
        super().setUp()
        package = self.directory / "toolchain"
        roots = create_native_package(self.executable, package)
        self.native_compiler = package / "bin" / executable_name("vela")
        self.native_environment = dict(os.environ, **roots)
        self.native_environment["PATH"] = ""
        self.source = self.write("main.vela", SOURCE)

    def test_jit_and_native_executable_agree_without_source(self) -> None:
        self.assertEqual(self.invoke("run", str(self.source)).stdout, "42\n")
        application = self.directory / executable_name("reflection")
        native_command(self, [self.native_compiler, "build", self.source, "-o", application],
                       env=self.native_environment, cwd=self.directory)
        self.source.unlink()
        result = native_command(self, [application], cwd=self.directory)
        self.assertEqual(result.stdout, "42\n")

    def test_object_and_static_library_expose_both_bindings(self) -> None:
        output = self.executable.parent
        libraries = native_test_libraries(output)
        includes = [ROOT, ROOT / "include", ROOT / "third_party/abseil-cpp",
                    ROOT / "third_party/googletest/googletest/include"]
        for emission in ("obj", "staticlib"):
            with self.subTest(emission=emission):
                suffix = (".obj" if sys.platform == "win32" else ".o") if emission == "obj" else ".lib" if sys.platform == "win32" else ".a"
                artifact = self.directory / ("reflection_" + emission + suffix)
                native_command(self, [self.native_compiler, "build", self.source,
                                      "--emit=" + emission, "--export=calculate", "-o", artifact],
                               env=self.native_environment, cwd=self.directory)
                manifest = json.loads(Path(str(artifact) + ".aot.json").read_text(encoding="utf-8"))
                support = manifest["target_support"]
                self.assertNotIn("link_arguments", support)
                if emission == "staticlib":
                    self.assertTrue(support["archive_arguments"])
                else:
                    self.assertNotIn("archive_arguments", support)
                host_source = self.write("host.cc", HOST.replace("IMAGE", manifest["image"])
                                         .replace("MODULE", json.dumps(self.source.as_posix())))
                executable = self.directory / executable_name("host_" + emission)
                compile_native_host(self, [host_source], executable, [artifact, *libraries],
                                    includes, cwd=self.directory)
                original = self.source.read_bytes()
                self.source.unlink()
                try:
                    self.assertIn("[  PASSED  ] 1 test.", native_command(self, [executable], cwd=self.directory).stdout)
                finally:
                    self.source.write_bytes(original)

    def test_offline_members_preserve_value_checks_and_single_execution_on_failure(self) -> None:
        source = self.write("members.vela", OFFLINE_MEMBER_SOURCE)
        suffix = ".obj" if sys.platform == "win32" else ".o"
        artifact = self.directory / ("members" + suffix)
        native_command(self, [self.native_compiler, "build", source, "--emit=obj",
                              "--export=exercise", "--export=fields", "--export=fail",
                              "--export=lookup", "--export=borrowed", "-o", artifact],
                       env=self.native_environment, cwd=self.directory)
        manifest = json.loads(Path(str(artifact) + ".aot.json").read_text(encoding="utf-8"))
        host = '#include "' + artifact.name + '.h"\n' + OFFLINE_MEMBER_HOST.replace(
            "REFLECTION_IMAGE", manifest["image"])
        host_source = self.write("members_host.cc", host)
        executable = self.directory / executable_name("members_host")
        libraries = native_test_libraries(self.executable.parent)
        includes = [ROOT, ROOT / "include", ROOT / "third_party/abseil-cpp",
                    ROOT / "third_party/googletest/googletest/include"]
        compile_native_host(self, [host_source], executable, [artifact, *libraries],
                            includes, cwd=self.directory)
        source.unlink()
        result = native_command(self, [executable], cwd=self.directory)
        self.assertIn("[  PASSED  ] 1 test.", result.stdout)


if __name__ == "__main__":
    unittest.main()
