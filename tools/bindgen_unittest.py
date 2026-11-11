"""Exercise the LibTooling binding collector against real translation units."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import unittest

from tools.clang_sdk import sdk_directory
from tools.bindgen import fingerprints, generate, verify_manifest
from tools.package import package_bindgen
from tools.config import ROOT, executable_name, static_library_name, toolchain_tools, xcode_path
from tools.test_support import CommandTestCase, compile_native_host, create_native_package


class BindingCollectorTest(CommandTestCase):
    directory_prefix = "vela-bindgen-"

    def setUp(self) -> None:
        super().setUp()
        self.collector = self.executable.parent / executable_name("vela_bindgen")
        self.compiler = toolchain_tools()["cxx"]
        self.resource = sdk_directory() / "lib/clang/21"
        self.header = self.directory / "接口 counter.h"
        self.source = self.write("input.cc", '#include "接口 counter.h"\n')
        flags = (["/nologo", "/clang:-std=c++23", "/c", "/Foignored.obj"]
                 if sys.platform == "win32" else ["-std=c++23", "-c", "-o", "ignored.o"])
        self.arguments = [str(self.compiler), *flags, "-I" + str(ROOT / "include"),
                          "-DHOST_BINDING_TEST=1", str(self.source)]
        self.platform_arguments = []
        if sys.platform == "darwin":
            sysroot = xcode_path(["--show-sdk-path"], "sysroot")
            self.platform_arguments = ["-isysroot", str(sysroot), "-nostdinc++", "-isystem",
                                       str(sysroot / "usr/include/c++/v1")]
            self.arguments[1:1] = self.platform_arguments
        self.database = self.directory / "compile_commands.json"
        self.write_database()

    def write_database(self, *, copies: int = 1) -> None:
        entry = {"directory": str(self.directory), "file": str(self.source),
                 "arguments": self.arguments}
        self.database.write_text(json.dumps([entry] * copies), encoding="utf-8", newline="\n")

    def collect(self, source: str, *, code: int = 0,
                extra: list[str] | None = None) -> subprocess.CompletedProcess:
        self.header.write_text('#pragma once\n#include <vela/bindings.h>\n' + source,
                               encoding="utf-8", newline="\n")
        output_arguments = extra or []
        if not any(argument.startswith("--output-directory=") for argument in output_arguments):
            output_arguments = ["--output-directory=" + str(self.directory / "collected"),
                                *output_arguments]
        result = subprocess.run([
            str(self.collector), "--module=app", "--database=" + str(self.database),
            "--source=" + str(self.source), "--header=" + str(self.header),
            "--resource-dir=" + str(self.resource), *output_arguments],
            cwd=self.directory, capture_output=True, text=True, encoding="utf-8", timeout=60,
            check=False)
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        if code:
            self.assertEqual(result.stdout, "", "failed collection must publish no model")
        return result

    def test_collects_public_declarations_names_locations_and_exact_types(self) -> None:
        source = """#include <cstdint>
namespace app {
using Count = std::int64_t;
/// A native counter.
class VELA_CLASS("app.Counter") Counter final {
 public:
  explicit Counter(Count initial) noexcept;
  VELA_NAME("add") Count Add(Count delta) noexcept;
  VELA_GETTER("value") Count value() const noexcept;
  VELA_SETTER("value") void Set(Count amount) noexcept;
  static double Scale(double factor) noexcept;
  bool Equal(const Counter& other) const noexcept;
  void Maybe(Counter* other) noexcept;
  VELA_IGNORE void Debug(int unsupported);
  VELA_IGNORE int ignored;
 private:
  void Hidden(int unsupported);
};
}
"""
        first = self.collect(source)
        model = json.loads(first.stdout)
        self.assertEqual(first.stdout, self.collect(source).stdout)
        self.assertEqual(model["clang_version"], "21.1.8")
        counter, = model["classes"]
        self.assertEqual(counter["key"], "app.Counter")
        self.assertEqual(counter["cpp"], "app::Counter")
        self.assertEqual(counter["documentation"], "A native counter.")
        self.assertEqual(Path(counter["source"]["path"]), self.header)
        self.assertEqual(counter["source"]["line"], 7)
        members = {member["key"]: member for member in counter["members"]}
        self.assertEqual(len(members), 7)
        add = members["app.Counter::method:add"]
        self.assertEqual(add["parameters"][0]["type"]["kind"], "int")
        self.assertEqual(add["result"]["kind"], "int")
        self.assertIn("noexcept", add["cpp_pointer_type"])
        self.assertTrue(members["app.Counter::get:value"]["const"])
        self.assertEqual(members["app.Counter::static:Scale"]["result"]["kind"], "double")
        borrow = members["app.Counter::method:Equal"]["parameters"][0]["type"]
        self.assertEqual((borrow["key"], borrow["ownership"], borrow["nullable"], borrow["const"]),
                         ("app.Counter", "borrow", False, True))
        self.assertTrue(members["app.Counter::method:Maybe"]["parameters"][0]["type"]["nullable"])
        self.assertIn(self.header.resolve(), [Path(path) for path in model["inputs"]])
        observed = model["input_fingerprints"]
        self.assertIn(str(self.header), observed)
        self.assertEqual(observed, fingerprints(self.collector, [Path(path) for path in observed]))
        self.assertFalse((self.directory / "ignored.obj").exists())

    def test_default_export_excludes_inheritance_and_implicit_lifecycle(self) -> None:
        model = json.loads(self.collect("""struct Base { void Hidden(int x); };
class VELA_CLASS("app.Derived") Derived : public Base {
 public:
  Derived(const Derived&) noexcept = default;
  Derived& operator=(const Derived&) noexcept = default;
  virtual void Ping() noexcept;
};
""").stdout)
        derived, = model["classes"]
        method, = derived["members"]
        self.assertEqual((method["name"], method["kind"]), ("Ping", "method"))

    def test_no_implicit_constructor_and_overloads_require_distinct_names(self) -> None:
        result = self.collect("""class VELA_CLASS("app.Counter") Counter {
 public:
  VELA_NAME("Int") void Add(bool x) noexcept;
  VELA_NAME("Double") void Add(double x) noexcept;
};""")
        members = json.loads(result.stdout)["classes"][0]["members"]
        self.assertEqual([member["name"] for member in members], ["Double", "Int"])

    def test_preserves_nested_local_contracts_and_generates_structured_types(self) -> None:
        source = """#include <cstdint>
#include <optional>
#include <vela/types.h>
namespace vela { template<class T> class Local; }
using Payload = vela::Array<std::int64_t>;
using Values = vela::Array<vela::Map<vela::String, vela::Nullable<vela::Future<Payload>>>>;
using Callback = vela::Function<vela::Future<Payload>(vela::Set<std::int64_t>)>;
class VELA_CLASS("app.Host") Host {
 public:
  vela::Local<Values> Echo(vela::Local<Values> values) noexcept;
  std::optional<vela::Local<Callback>> Select(std::optional<vela::Local<Callback>> callback) noexcept;
};"""
        output = self.directory / "generated"
        model = json.loads(self.collect(source, extra=["--output-directory=" + str(output)]).stdout)
        self.assertEqual(model["format_version"], 2)
        host, = model["classes"]
        members = {member["name"]: member for member in host["members"]}
        array = members["Echo"]["result"]
        self.assertEqual((array["kind"], array["ownership"]), ("array", "local"))
        future = array["element"]["value_type"]
        self.assertEqual((future["kind"], future["nullable"]), ("future", True))
        self.assertEqual(future["result"]["element"]["kind"], "int")
        callback = members["Select"]["parameters"][0]["type"]
        self.assertTrue(callback["nullable"])
        self.assertEqual(callback["parameters"][0]["kind"], "set")
        emitted = model["products"]["app.vela"]
        self.assertIn("Map<string, Future<int[]>?>[]", emitted)
        self.assertIn("Set<int>", emitted)
        changed = json.loads(self.collect(source.replace("Array<std::int64_t>", "Array<double>")).stdout)
        self.assertNotEqual(host["contract"], changed["classes"][0]["contract"])

    def test_maps_owned_results_strings_context_and_failures(self) -> None:
        model = json.loads(self.collect("""#include <memory>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
namespace vela { struct Error {}; }
namespace vela::runtime { class NativeContext; class CancellationToken {}; }
class VELA_CLASS("app.Counter") Counter {
 public:
  explicit Counter(vela::runtime::NativeContext& context, bool initial) noexcept;
  static std::expected<std::unique_ptr<Counter>, vela::Error>
      Create(vela::runtime::NativeContext& context) noexcept;
  std::shared_ptr<Counter> Shared() noexcept;
  std::string Text(std::string_view input) const noexcept;
  std::optional<std::string> MaybeText() noexcept;
  std::expected<void, vela::Error> Reset() noexcept;
  vela::runtime::CancellationToken Cancel(vela::runtime::CancellationToken token) noexcept;
  template<class T> VELA_IGNORE void Unbound(T value);
};""").stdout)
        members = {member["name"]: member for member in model["classes"][0]["members"]}
        constructor = members["constructor"]
        self.assertTrue(constructor["context"])
        self.assertEqual([p["name"] for p in constructor["parameters"]], ["initial"])
        self.assertFalse(constructor["result"]["nullable"])
        factory = members["Create"]
        self.assertTrue(factory["context"])
        self.assertTrue(factory["fallible"])
        self.assertEqual(factory["result"]["ownership"], "unique")
        self.assertTrue(factory["result"]["nullable"])
        self.assertEqual(members["Shared"]["result"]["ownership"], "shared")
        self.assertEqual(members["Text"]["parameters"][0]["type"]["kind"], "string")
        self.assertEqual(members["Text"]["result"]["kind"], "string")
        self.assertTrue(members["MaybeText"]["result"]["nullable"])
        self.assertEqual(members["Reset"]["result"]["kind"], "void")
        self.assertTrue(members["Reset"]["fallible"])
        self.assertEqual(members["Cancel"]["result"]["kind"], "CancellationToken")

    def test_generated_script_header_round_trips_nominals_generics_and_source_dependencies(self) -> None:
        dependency = self.write("types.vela", "export class Box<T> { value: T; private hidden: int = 0; }\n")
        entry = self.write("api.vela", "import 'types.vela' as types;\n"
                           "func fail(): int { throw Error(message: 'must not run'); }\n"
                           "const initialized = fail();\n"
                           "export func echo(value: types.Box<int>): types.Box<int> { return value; }\n")
        header = self.directory / "script.h"
        self.invoke("host-header", str(entry), "--namespace=script", "-o", str(header))
        generated = header.read_text(encoding="utf-8")
        self.assertIn("using Arguments = std::tuple<std::int64_t>", generated)
        self.assertNotIn("Get_hidden", generated)
        self.arguments[1:1] = ["-I" + str(ROOT), "-I" + str(ROOT / "third_party/abseil-cpp")]
        self.write_database()
        # Read the exact generated signature through the function type, without spelling tag names.
        self.collect('''#include "script.h"
template<class T> struct ResultOf;
template<class T> struct ResultOf<vela::Entry<T(T)>> { using Type = T; };
using Box = typename ResultOf<script::Entry_echo>::Type;
class VELA_CLASS("app.Service") Service final { public:
  vela::Local<Box> Echo(vela::Local<Box> value) noexcept { return value; }
};
''')
        manifest_path = generate(self.binding_config(), self.collector, self.resource)
        manifest = verify_manifest(manifest_path)
        shape = manifest["bindings"]["classes"][0]["members"][0]["result"]
        self.assertEqual((Path(shape["module"]), shape["declaration"]), (dependency, "Box"))
        self.assertEqual(shape["arguments"][0]["kind"], "int")
        self.assertIn(str(dependency), manifest["inputs"])
        self.assertIn("scriptDependency1.Box<int>", (manifest_path.parent / "app.vela").read_text())
        standard = "/clang:-std=c++23" if sys.platform == "win32" else "-std=c++23"
        flags = ["/nologo", "/Zs"] if sys.platform == "win32" else ["-fsyntax-only"]
        compiled = subprocess.run([str(self.compiler), *self.platform_arguments, standard, *flags,
                                   "-I" + str(ROOT), "-I" + str(ROOT / "include"),
                                   "-I" + str(ROOT / "third_party/abseil-cpp"),
                                   str(manifest_path.parent / "app_bindings.cc")],
                                  capture_output=True, text=True, check=False)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        dependency.write_text("export class Box<T> { value: T; }\n", encoding="utf-8", newline="\n")
        with self.assertRaisesRegex(ValueError, "Stale binding input"):
            verify_manifest(manifest_path)

    def test_host_header_rejects_private_public_dependencies_without_replacing_output(self) -> None:
        source = self.write("private.vela", "class Secret {} export func create(): Secret { return Secret(); }")
        output = self.write("private.h", "previous generation\n")
        rejected = self.invoke("host-header", str(source), "--namespace=script", "-o", str(output), code=65)
        self.assertIn("not exported", rejected.stderr)
        self.assertEqual(output.read_text(), "previous generation\n")

    def test_rejects_unowned_results_and_nonstandard_stl_specializations(self) -> None:
        prefix = '#include <memory>\n#include <string>\n'
        cases = [
            'const Counter& Result() noexcept;',
            'Counter Result() noexcept;',
            'void Input(std::shared_ptr<Counter> value) noexcept;',
            'void Input(std::unique_ptr<Counter> value) noexcept;',
            'struct Deleter { void operator()(Counter*) noexcept; }; '
            'std::unique_ptr<Counter, Deleter> Result() noexcept;',
            'struct Traits : std::char_traits<char> {}; '
            'std::basic_string<char, Traits> Result() noexcept;',
        ]
        for declaration in cases:
            with self.subTest(declaration=declaration):
                result = self.collect(prefix + 'class VELA_CLASS("app.Counter") Counter { public: ' +
                                      declaration + ' };', code=1)
                self.assertIn("unsupported", result.stderr)

    def test_uses_real_defines_and_reports_ambiguous_compile_commands(self) -> None:
        source = """#if HOST_BINDING_TEST != 1
#error missing translation unit definition
#endif
class VELA_CLASS("app.Counter") Counter {};"""
        self.collect(source)
        self.write_database(copies=2)
        self.assertIn("exactly one compilation command", self.collect(source, code=1).stderr)

    def test_expands_response_files_and_tracks_their_dependencies(self) -> None:
        response = self.write("flags.rsp", '-DRESPONSE_INPUT=1\n')
        self.arguments.insert(1, "@" + str(response))
        self.write_database()
        model = json.loads(self.collect("""#if RESPONSE_INPUT != 1
#error response file was not used
#endif
class VELA_CLASS("app.Counter") Counter {};""").stdout)
        self.assertIn(response, [Path(path) for path in model["inputs"]])
        self.assertIn("-DRESPONSE_INPUT=1", model["arguments"])

    def test_rejects_unsupported_public_contracts(self) -> None:
        cases = [
            ('void Work();', "noexcept"),
            ('void Work(int value) noexcept;', "unsupported parameter"),
            ('int Work() noexcept;', "unsupported result"),
            ('void Work(bool value = false) noexcept;', "default arguments"),
            ('consteval bool Work() const noexcept { return true; }', "runtime-callable"),
            ('consteval Counter() noexcept {}', "runtime-callable"),
            ('bool Work(this const Counter& self) noexcept;', "explicit object parameter"),
            ('void Work(bool) noexcept;', "parameter requires"),
            ('int field;', "public fields"),
            ('Counter() noexcept; Counter(bool value) noexcept;', "at most one"),
            ('void Work(bool value) noexcept; void Work(double value) noexcept;', "conflicting"),
            ('VELA_GETTER("x") void Work() noexcept;', "getter requires"),
            ('VELA_SETTER("x") bool Work(bool value) noexcept;', "setter requires"),
            ('virtual void Work() noexcept = 0;', "pure virtual"),
            ('Counter* Work() noexcept;', "unsupported result"),
            ('template<class T> void Work(T value) noexcept;', "member templates"),
            ('VELA_NAME("x") VELA_GETTER("x") bool Work() noexcept;', "one Vela annotation"),
            ('void Work(VELA_NAME("repeat") bool a, VELA_NAME("repeat") bool b) noexcept;',
             "parameter requires"),
        ]
        for declarations, diagnostic in cases:
            with self.subTest(declarations=declarations):
                result = self.collect('class VELA_CLASS("app.Counter") Counter { public: ' +
                                      declarations + ' };', code=1)
                self.assertIn(diagnostic, result.stderr)
                self.assertIn(self.header.name, result.stderr)
                self.assertRegex(result.stderr, r"counter\.h(?::3:|\(3,)")

    def test_normal_compilation_annotations_have_no_layout_or_behavior(self) -> None:
        self.header.write_text("""#include <vela/bindings.h>
class VELA_CLASS("app.Counter") Counter { public:
  VELA_NAME("add") void Add() noexcept;
  VELA_GETTER("value") bool value() const noexcept;
  VELA_IGNORE int field;
};
static_assert(sizeof(Counter) == sizeof(int));
#if defined(VELA_BINDGEN)
#error annotations must be absent in host builds
#endif
""", encoding="utf-8", newline="\n")
        flags = ["/Zs", "/nologo"] if sys.platform == "win32" else ["-fsyntax-only"]
        result = subprocess.run([str(self.compiler), *self.platform_arguments, *flags,
                                 "-I" + str(ROOT / "include"),
                                 str(self.source)], capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def binding_config(self, module: str = "app", *, dependencies: list[str] | None = None) -> Path:
        return self.write(module + ".json", json.dumps({
            "module": module, "headers": [self.header.name],
            "translation_unit": self.source.name,
            "compilation_database": self.database.name, "output": "generated/" + module,
            "dependencies": dependencies or []}))

    def test_publishes_checked_deterministic_dual_output_and_rejects_mixed_generation(self) -> None:
        self.collect("""#include <cstdint>
class VELA_CLASS("app.Counter") Counter final { public:
  explicit Counter(std::int64_t initial) noexcept;
  VELA_NAME("add") std::int64_t Add(std::int64_t delta) noexcept;
  VELA_GETTER("value") std::int64_t Value() const noexcept;
};""")
        config = self.binding_config()
        manifest = generate(config, self.collector, self.resource)
        output = manifest.parent
        self.assertEqual(verify_manifest(manifest)["registration"], "vela::bindings::module_app::Register")
        self.assertIn("export external class Counter", (output / "app.vela").read_text())
        self.assertIn("static_cast<NativeThunk", (output / "app_bindings.cc").read_text())
        before = {path.name: (path.read_bytes(), path.stat().st_mtime_ns) for path in output.iterdir()}
        generate(config, self.collector, self.resource, check=True)
        generate(config, self.collector, self.resource)
        self.assertEqual(before, {path.name: (path.read_bytes(), path.stat().st_mtime_ns)
                                  for path in output.iterdir()})
        standard = "/clang:-std=c++23" if sys.platform == "win32" else "-std=c++23"
        compile_arguments = [str(self.compiler), *self.platform_arguments, standard, "-I" + str(ROOT),
                             "-I" + str(ROOT / "include"),
                             "-I" + str(ROOT / "third_party/abseil-cpp")]
        compile_arguments += ["/nologo", "/Zs"] if sys.platform == "win32" else ["-fsyntax-only"]
        compiled = subprocess.run([*compile_arguments, str(output / "app_bindings.cc")],
                                  capture_output=True, text=True, check=False)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        script = output / "app.vela"
        self.invoke("check", str(script))
        self.invoke("fmt", "--check", str(script))
        documentation = self.directory / "bindings.md"
        self.invoke("doc", str(script), str(documentation))
        self.assertIn("external class Counter = 'app.Counter'", documentation.read_text(encoding="utf-8"))
        generated_cpp = output / "app_bindings.cc"
        original_cpp = generated_cpp.read_bytes()
        generated_cpp.write_bytes(original_cpp + b"// mixed\n")
        rejected = self.invoke("check", str(script), code=65)
        self.assertIn("stale or mixed binding product", rejected.stderr)
        generated_cpp.write_bytes(original_cpp)
        (output / "app.vela").write_text("// broken generation\n", encoding="utf-8", newline="\n")
        with self.assertRaisesRegex(ValueError, "Stale or mixed"):
            verify_manifest(manifest)
        with self.assertRaisesRegex(ValueError, "missing or stale"):
            generate(config, self.collector, self.resource, check=True)

    def test_generates_imports_and_traits_for_dependency_host_types(self) -> None:
        self.collect('class VELA_CLASS("base.Counter") Counter {};')
        base_manifest = generate(self.binding_config("base"), self.collector, self.resource)
        other = self.write("other.h", self.header.read_text(encoding="utf-8"))
        self.header.write_text('''#include <vela/bindings.h>
#include "other.h"
class VELA_CLASS("app.Service") Service { public:
  void Accept(const Counter& counter) noexcept;
};
''', encoding="utf-8", newline="\n")
        config = self.binding_config(dependencies=[str(base_manifest)])
        # The dependency's source header is an input snapshot and must remain unchanged.
        base_config = json.loads((self.directory / "base.json").read_text())
        base_config["headers"] = [other.name]
        base_source = self.write("base.cc", '#include "other.h"\n')
        base_config["translation_unit"] = base_source.name
        database = self.write("base_commands.json", json.dumps([{
            "directory": str(self.directory), "file": str(base_source),
            "arguments": [*self.arguments[:-1], str(base_source)]}]))
        base_config["compilation_database"] = database.name
        self.write("base.json", json.dumps(base_config))
        generate(self.directory / "base.json", self.collector, self.resource)
        manifest = generate(config, self.collector, self.resource)
        script = (manifest.parent / "app.vela").read_text()
        self.assertIn("nativeDependency0.Counter", script)
        self.assertIn("other.h", (base_manifest.parent / "base_bindings.h").read_text())

    def test_packaged_binding_command_uses_only_bundled_parser_resource_and_formatter(self) -> None:
        self.collect('class VELA_CLASS("app.Counter") Counter { public: void Ping() noexcept; };')
        package = self.directory / "package"
        package_bindgen(self.collector, package)
        config = self.binding_config()
        result = subprocess.run([sys.executable, str(package / "bin/vela-bindgen.py"),
                                 "--config", str(config)], cwd=self.directory, capture_output=True,
                                text=True, encoding="utf-8", check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        manifest = self.directory / "generated/app/app.bindings.json"
        self.assertTrue(verify_manifest(manifest))
        parsed = json.loads(manifest.read_text())
        self.assertEqual(parsed["bindings"]["clang_version"], "21.1.8")

    def test_rejects_unnameable_abstract_or_conflicting_class_exports_at_source(self) -> None:
        cases = [
            ('namespace { class VELA_CLASS("app.Hidden") Hidden {}; }', "externally visible"),
            ('class VELA_CLASS("app.int") Integer {};', "non-builtin"),
            ('class VELA_CLASS("first.Same") First {}; '
             'class VELA_CLASS("second.Same") Second {};', "duplicate module export"),
            ('class VELA_CLASS("app.Abstract") Abstract { public: Abstract() noexcept; '
             'VELA_IGNORE virtual void Work() = 0; };', "abstract"),
            ('class VELA_CLASS("app.Forward") Forward;', "complete class definition"),
            ('template<class T> struct Outer { '
             'class VELA_CLASS("app.Inner") Inner {}; };', "non-template"),
            ('class VELA_CLASS("app.Copy") Copy { public: VELA_NAME("clone") '
             'Copy(const Copy&) noexcept; };', "lifecycle methods"),
        ]
        for source, diagnostic in cases:
            with self.subTest(source=source):
                self.assertIn(diagnostic, self.collect(source, code=1).stderr)

    def test_registration_names_are_valid_for_cpp_keywords_and_reserved_identifier_spelling(self) -> None:
        self.collect('class VELA_CLASS("app.Counter") Counter {};')
        for module in ("namespace", "_Z_"):
            with self.subTest(module=module):
                manifest = generate(self.binding_config(module), self.collector, self.resource)
                registration = verify_manifest(manifest)["registration"]
                self.assertTrue(registration.startswith("vela::bindings::module_"))
                self.assertNotIn("__", registration)

    @unittest.skipUnless(sys.platform == "win32" or os.environ.get("VELA_TEST_TARGET_SUPPORT_SPEC"),
                         "Native distribution acceptance requires an audited VELA_TEST_TARGET_SUPPORT_SPEC")
    def test_aot_exports_use_typed_rooted_host_handles_and_run_without_source(self) -> None:
        package = self.directory / "aot_package"
        create_native_package(self.executable, package)
        runtime = package / json.loads((package / "share/vela/target.json").read_text())["runtime"]
        self.arguments.remove("-I" + str(ROOT / "include"))
        self.arguments[1:1] = ["-I" + str(package / "include")]
        self.write_database()
        self.header.write_text('''#pragma once
#include <vela/bindings.h>
#include <cstdint>
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_context.h"
namespace host {
class VELA_CLASS("app.Counter") Counter final {
 public:
  explicit Counter(std::int64_t initial) noexcept : value_(initial) {}
  std::int64_t Collect(vela::runtime::NativeContext& context, std::int64_t delta) noexcept {
    context.isolate().heap().Collect(); return value_ += delta;
  }
 private:
  std::int64_t value_;
};
}
''', encoding="utf-8", newline="\n")
        generated = subprocess.run([sys.executable, str(package / "bin/vela-bindgen.py"),
                                    "--config", str(self.binding_config())], cwd=self.directory,
                                   capture_output=True, text=True, encoding="utf-8", check=False)
        self.assertEqual(generated.returncode, 0, generated.stdout + generated.stderr)
        manifest = self.directory / "generated/app/app.bindings.json"
        self.assertTrue(verify_manifest(manifest))
        script = self.write("exports.vela", """import 'generated/app/app';
export func create(initial: int): Counter { return Counter(initial); }
export func update(counter: Counter): int { return counter.Collect(2); }
export func retain(counter: Counter?): Counter? { return counter; }
""")
        compiler = package / "bin" / executable_name("vela")
        for kind in ("obj", "staticlib"):
            with self.subTest(kind=kind):
                library = self.directory / ("image" + (".obj" if sys.platform == "win32" else ".o")
                                             if kind == "obj" else static_library_name("image"))
                built = subprocess.run([str(compiler), "build", str(script), "--emit=" + kind,
                                        "--export=create", "--export=update", "--export=retain",
                                        "-o", str(library)], cwd=self.directory, capture_output=True,
                                       text=True, encoding="utf-8", check=False)
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                metadata = json.loads(Path(str(library) + ".aot.json").read_text())
                namespace = metadata["image"].removesuffix("_image")
                header = Path(str(library) + ".h").read_text()
                self.assertIn("vela::Host<", header)
                self.assertIn("vela::Nullable<vela::Host<", header)
                host = self.write("host_main.cc", '''#include "IMAGE_HEADER"
#include <cassert>
#include "generated/app/app_bindings.h"
#include "runtime/heap/heap.h"
int main() {
  vela::runtime::Isolate isolate(1);
  if (!vela::bindings::module_app::Register(isolate)) return 1;
  auto instance = IMAGE_NAMESPACE::Instantiate(&isolate);
  if (!instance) return 2;
  auto retained = [&] {
    vela::HandleScope scope(isolate);
    auto counter = IMAGE_NAMESPACE::Call_create(scope, instance->get(), 40);
    assert(counter.has_value());
    auto returned = IMAGE_NAMESPACE::Call_retain(scope, instance->get(), *counter);
    assert(returned.has_value() && returned->has_value());
    return (*returned)->Persist();
  }();
  if (!retained) return 3;
  instance->reset();
  isolate.heap().Collect();
  instance = IMAGE_NAMESPACE::Instantiate(&isolate);
  if (!instance) return 5;
  vela::HandleScope scope(isolate);
  const auto counter = retained->ToLocal(scope);
  if (!counter) return 4;
  retained->Reset();
  const auto result = IMAGE_NAMESPACE::Call_update(scope, instance->get(), *counter);
  return result && *result == 42 ? 0 : 6;
}
'''.replace("IMAGE_HEADER", library.name + ".h").replace("IMAGE_NAMESPACE", namespace))
                executable = self.directory / executable_name("host_main")
                includes = [package / "include"]
                generated = manifest.parent / "app_bindings.cc"
                compile_native_host(self, [host, generated], executable, [library, runtime],
                                    includes, cwd=self.directory)
                # 独立目录及空 PATH 验证运行时不查找源码、清单、Clang 或编译器。
                backup = script.read_bytes()
                script.unlink()
                execution_directory = self.directory / ("standalone_" + kind)
                execution_directory.mkdir()
                standalone = execution_directory / executable.name
                executable.replace(standalone)
                environment = dict(os.environ, PATH="")
                ran = subprocess.run([str(standalone)], cwd=execution_directory, env=environment,
                                     capture_output=True, text=True, check=False)
                self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
                script.write_bytes(backup)


if __name__ == "__main__":
    unittest.main()
