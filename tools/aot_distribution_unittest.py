"""Exercise packaged builds, native process semantics, publication, and shared runtime linking."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

from tools.config import ROOT, executable_name, static_library_name, toolchain_tools
from tools.test_support import native_command, vela_executable
from tools.package import create_package


@unittest.skipUnless(sys.platform == "win32" or os.environ.get("VELA_TEST_TARGET_SUPPORT_SPEC"),
                     "Native distribution acceptance requires an audited VELA_TEST_TARGET_SUPPORT_SPEC")
class NativeDistributionTest(unittest.TestCase):
    def package(self, original: Path, destination: Path) -> dict[str, str]:
        specification = os.environ.get("VELA_TEST_TARGET_SUPPORT_SPEC")
        return create_package(original, original.parent / "obj/runtime" /
                              static_library_name("vela_runtime"), destination,
                              Path(specification) if specification else None)

    def test_existing_language_corpus_matches_vm(self) -> None:
        categories = [
            "class", "interface", "generic", "global", "exception", "dynamic_array",
            "double", "numeric_conversion", "byte_data", "collection", "function_value",
            "builtin_function_value", "string", "regexp", "json", "standard_library", "async_contract",
            "language_basics", "control_flow", "match", "cast", "optimization", "recursion",
        ]
        selected = os.environ.get("VELA_AOT_CORPUS_CATEGORIES")
        if selected:
            requested = selected.split(",")
            self.assertFalse(set(requested) - set(categories), "unknown corpus category")
            categories = requested
        with tempfile.TemporaryDirectory(prefix="vela-语言对照 ") as temporary:
            work = Path(temporary)
            original = vela_executable()
            package = work / "toolchain"
            roots = self.package(original, package)
            environment = dict(os.environ, **roots)
            environment["PATH"] = ""
            compiler = package / "bin" / executable_name("vela")
            compared = 0
            for category in categories:
                contents = (ROOT / "compiler" / (category + "_unittest.cc")).read_text(encoding="utf-8")
                cases = re.finditer(r'R"(\w*)\((.*?)\)\1"', contents, re.DOTALL)
                count = 0
                for index, match in enumerate(cases):
                    source_text = match.group(2)
                    # 自定义宿主绑定由 standalone native host 测试覆盖；本矩阵覆盖 CLI 能直接运行的程序。
                    if not re.search(r"\bfunc main\(\)", source_text) or re.search(r"\bexternal\s+func\b", source_text):
                        continue
                    with self.subTest(category=category, case=index):
                        source = work / f"{category}_{index}.vela"
                        source.write_text(source_text, encoding="utf-8", newline="\n")
                        vm = subprocess.run([str(original), "run", str(source)], cwd=work,
                                            capture_output=True, text=True, encoding="utf-8", timeout=30)
                        application = work / executable_name("app")
                        built = subprocess.run([str(compiler), "build", str(source), "-o", str(application)],
                                               cwd=work, env=environment, capture_output=True,
                                               text=True, encoding="utf-8", timeout=30)
                        if vm.returncode == 65:
                            self.assertEqual(built.returncode, 65, built.stderr)
                            self.assertEqual(built.stderr, vm.stderr)
                        else:
                            self.assertEqual(built.returncode, 0, built.stderr)
                            native = subprocess.run([str(application)], cwd=work, env=environment,
                                                    capture_output=True, text=True, encoding="utf-8", timeout=30)
                            self.assertEqual(native.returncode, vm.returncode, native.stderr + vm.stderr)
                            self.assertEqual(native.stdout, vm.stdout)
                            self.assertEqual(built.stderr + native.stderr, vm.stderr)
                        source.unlink()
                        count += 1
                        compared += 1
                print(f"AOT/VM corpus: {category}: {count} cases", flush=True)
            if not selected:
                self.assertGreater(compared, 350)

    def test_packaged_builds_and_source_free_execution(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vela-发行包 ") as temporary:
            work = Path(temporary)
            original = vela_executable()
            output = original.parent
            package = work / "toolchain 中"
            roots = self.package(original, package)
            environment = {key: value for key, value in os.environ.items()
                           if key.upper() not in {"PATH", "INCLUDE", "LIB", "LIBPATH", "VCTOOLSINSTALLDIR", "WINDOWSSDKDIR"}}
            environment.update(roots)
            environment["PATH"] = ""
            compiler = package / "bin" / executable_name("vela")

            def run(arguments: list[str | Path], expected: int = 0,
                    env: dict[str, str] | None = environment) -> subprocess.CompletedProcess:
                return native_command(self, arguments, expected, env, cwd=work)

            fixtures = [
                ("scalar", "func main(): int { return 42; }", 42),
                ("void", "func main(): void { print('ready'); }", 0),
                ("await", "async func main(): Future<int> { print('first'); await 0; print('last'); return 42; }", 42),
                ("timer", "async func main(): Future<int> { Timer.after(0, (): void => { print('timer'); }); return 42; }", 42),
                ("composition", "async func main(): Future<int> { return await Future.value(20).then((v: int): int => v + 22); }", 42),
                ("idle", "func main(): Future<int> { return Future.race<int>([]); }", 70),
                ("background", "func main(): int { Future.microtask((): void => { throw Error(message: 'background'); }); return 0; }", 70),
                ("failure", "func main(): int { return 1 / 0; }", 70),
                ("async_failure", "async func fail(): Future<int> { await 0; return 1 / 0; }\nasync func main(): Future<int> { return await fail(); }", 70),
                ("invalid_type", "func main(): bool { return true; }", 70),
                ("invalid_range", "func main(): int { return 256; }", 70),
                ("standard", "func main(): int { return Math.sqrt(1764.0).toInt(); }", 42),
            ]
            applications = []
            sources = []
            for name, text, expected in fixtures:
                with self.subTest(name=name):
                    source = work / (name + " 中.vela")
                    source.write_text(text + "\n", encoding="utf-8", newline="\n")
                    sources.append(source)
                    vm = run([original, "run", source], expected, None)
                    app = work / executable_name(name + " 原生")
                    run([compiler, "build", source, "-o", app])
                    native = run([app], expected)
                    self.assertEqual(native.stdout, vm.stdout)
                    self.assertEqual(native.stderr, vm.stderr)
                    applications.append((app, expected, native.stdout, native.stderr))

            libraries = []
            manifests = []
            for name, source_text in [
                ("numbers", "export func calculate(value: int): int { return value + 2; }"),
                ("strings", "export func calculate(value: string): string { return value + '!'; }")]:
                source = work / (name + ".vela")
                source.write_text(source_text, encoding="utf-8", newline="\n")
                sources.append(source)
                library = work / static_library_name(name)
                run([compiler, "build", source, "--emit=staticlib", "--export=calculate", "-o", library])
                manifest = Path(str(library) + ".aot.json")
                run([compiler, "build", "--verify-manifest=" + str(manifest)])
                libraries.append(library)
                manifests.append(json.loads(manifest.read_text(encoding="utf-8")))
            tools = toolchain_tools()
            host_source = work / "embedding.cc"
            namespaces = [manifest["image"].removesuffix("_image") for manifest in manifests]
            host_source.write_text(r'''
#include <memory>
#include <string_view>
#include <gtest/gtest.h>
#include "NUMBERS_HEADER"
#include "STRINGS_HEADER"
TEST(PackageEmbeddingTest, TwoArchivesShareOneRuntimeAndKeepStringResultsAlive) {
  auto isolate = std::unique_ptr<VelaAotIsolate, decltype(&vela_aot_v1_isolate_release)>(
      vela_aot_v1_isolate_create(), vela_aot_v1_isolate_release);
  ASSERT_NE(isolate, nullptr);
  auto numbers = NUMBERS::Instantiate(isolate.get());
  auto strings = STRINGS::Instantiate(isolate.get());
  ASSERT_TRUE(numbers.has_value());
  ASSERT_TRUE(strings.has_value());
  const auto scalar = NUMBERS::Call_calculate(numbers->get(), 40);
  ASSERT_TRUE(scalar.has_value());
  EXPECT_EQ(*scalar, 42);
  auto input = vela::runtime::aot::CreateString(isolate.get(), "你好");
  ASSERT_TRUE(input.has_value());
  auto text = STRINGS::Call_calculate(strings->get(), *input);
  ASSERT_TRUE(text.has_value());
  input = vela::runtime::aot::String{};
  strings->reset();
  const char* bytes = nullptr;
  std::uint64_t size = 0;
  ASSERT_EQ(vela_aot_v1_string_view(text->get(), &bytes, &size), VELA_AOT_RETURNED);
  EXPECT_EQ(std::string_view(bytes, size), "你好!");
}
'''.replace("NUMBERS_HEADER", libraries[0].name + ".h")
                .replace("STRINGS_HEADER", libraries[1].name + ".h")
                .replace("NUMBERS", namespaces[0]).replace("STRINGS", namespaces[1]), encoding="utf-8", newline="\n")
            host = work / executable_name("embedding")
            runtime = package / json.loads((package / "share/vela/target.json").read_text())["runtime"]
            gtest = [output / "obj/third_party" / static_library_name(name)
                     for name in ("gtest_main", "gtest")]
            includes = [package / "include", ROOT / "third_party/googletest/googletest/include"]
            if sys.platform == "win32":
                host_object = work / "embedding.obj"
                run([tools["cxx"], "/nologo", "/MT", "/O2", "/clang:-std=c++23",
                 "/D_ITERATOR_DEBUG_LEVEL=0", "/D_HAS_EXCEPTIONS=0", "/c", host_source,
                 "/Fo" + str(host_object), *["/I" + str(path) for path in includes]], env=None)
                run([tools["cxx"], "/nologo", "/MT", "-fuse-ld=lld", "/Fe" + str(host), host_object,
                     *libraries, runtime, *gtest], env=None)
            else:
                flags = ["-std=c++23", "-O2", "-stdlib=libc++", "-fno-exceptions", "-pthread"]
                if sys.platform == "darwin":
                    flags += ["-fuse-ld=" + str(tools["linker"]),
                              "-isysroot", str(tools["sysroot"]), "-nostdinc++",
                              "-isystem", str(tools["sysroot"] / "usr/include/c++/v1"),
                              "-framework", "CoreFoundation"]
                run([tools["cxx"], *flags, *["-I" + str(path) for path in includes],
                     host_source, *libraries, runtime, *gtest, "-o", host], env=None)
            self.assertIn("[  PASSED  ] 1 test.", run([host]).stdout)

            header = Path(str(libraries[0]) + ".h")
            original_header = header.read_bytes()
            header.write_bytes(original_header + b"// another generation\n")
            self.assertIn("different generation", run([compiler, "build", "--verify-manifest=" +
                          str(libraries[0]) + ".aot.json"], 65).stderr)
            header.write_bytes(original_header)

            previous_library = libraries[0].read_bytes()
            header.unlink()
            header.mkdir()
            replacement = work / "replacement.vela"
            replacement.write_text("export func calculate(value: int): int { return value + 3; }", encoding="utf-8")
            sources.append(replacement)
            run([compiler, "build", replacement, "--emit=staticlib", "--export=calculate", "-o", libraries[0]], 74)
            self.assertIn("different generation", run([compiler, "build", "--verify-manifest=" +
                          str(libraries[0]) + ".aot.json"], 65).stderr)
            header.rmdir()
            header.write_bytes(original_header)
            libraries[0].write_bytes(previous_library)

            old_app = applications[0][0].read_bytes()
            invalid = work / "invalid.vela"
            invalid.write_text("func main(): int { return true; }", encoding="utf-8")
            sources.append(invalid)
            run([compiler, "build", invalid, "-o", applications[0][0]], 65)
            self.assertEqual(applications[0][0].read_bytes(), old_app)
            target_path = package / "share/vela/target.json"
            target = json.loads(target_path.read_text(encoding="utf-8"))
            broken = dict(target, link_arguments=["/invalid-link-option"])
            target_path.write_text(json.dumps(broken), encoding="utf-8")
            run([compiler, "build", sources[0], "-o", applications[0][0]], 74)
            self.assertEqual(applications[0][0].read_bytes(), old_app)
            target_path.write_text(json.dumps(dict(target, runtime_abi="wrong")), encoding="utf-8")
            self.assertIn("ABI", run([compiler, "build", sources[0], "-o", applications[0][0]], 74).stderr)
            target_path.write_text(json.dumps(target), encoding="utf-8")

            # obj 与 staticlib 不需要最终平台 SDK；exe 缺失 SDK 必须在读取源码前诊断。
            target_roots = [entry["environment"] for entry in target["external_roots"]]
            without_sdk = {key: value for key, value in environment.items() if key not in roots}
            without_sdk = {key: value for key, value in without_sdk.items() if key not in target_roots}
            run([compiler, "build", sources[0], "--emit=obj", "-o", work / "sdkless.obj"], env=without_sdk)
            run([compiler, "build", sources[0], "--emit=staticlib", "-o", work / "sdkless.lib"], env=without_sdk)
            if target_roots:
                self.assertIn(target_roots[0], run([compiler, "build", work / "missing.vela", "-o",
                              work / executable_name("missing")], 74, without_sdk).stderr)

            custom = work / "custom.vela"
            custom.write_text("external func host(): int; func main(): int { return host(); }", encoding="utf-8")
            sources.append(custom)
            self.assertIn("custom host binding", run([compiler, "build", custom, "-o", work / "custom.exe"], 65).stderr)

            # 删除源码及整个 Vela 发行包后，产物仍独立运行。
            for source in sources:
                source.unlink()
            shutil.rmtree(package)
            for app, expected, stdout, stderr in applications:
                result = run([app], expected)
                self.assertEqual((result.stdout, result.stderr), (stdout, stderr))
            self.assertIn("[  PASSED  ] 1 test.", run([host]).stdout)


if __name__ == "__main__":
    unittest.main()
