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
from tools.test_support import (closed_output_command, compile_native_host, create_native_package,
                                native_command, native_test_libraries, pending_command,
                                vela_executable)
from tools.aot_c_fixture import host_source as c_host_source
from tools.program_fixtures import (OUTPUT_FAILURE_SOURCE, PENDING_OUTPUT_PREFIX, PENDING_SOURCE,
                                    PROGRAM_CASES)


@unittest.skipUnless(sys.platform == "win32" or os.environ.get("VELA_TEST_TARGET_SUPPORT_SPEC"),
                     "Native distribution acceptance requires an audited VELA_TEST_TARGET_SUPPORT_SPEC")
class NativeDistributionTest(unittest.TestCase):
    def test_existing_language_corpus_matches_jit(self) -> None:
        categories = [
            "class", "interface", "generic", "generic_method", "test_sources", "global", "exception", "dynamic_array",
            "double", "numeric_conversion", "byte_data", "collection", "function_value",
            "builtin_function_value", "string", "regexp", "json", "typed_json", "standard_library", "async_contract",
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
            roots = create_native_package(original, package)
            environment = dict(os.environ, **roots)
            environment["PATH"] = ""
            compiler = package / "bin" / executable_name("vela")
            compared = 0
            for category in categories:
                if category == "test_sources":
                    corpus = ROOT / "compiler/test_support/test_sources.h"
                elif category == "optimization":
                    corpus = ROOT / "compiler/language/optimized_execution_unittest.cc"
                else:
                    corpus = ROOT / "compiler/language" / (category + "_unittest.cc")
                contents = corpus.read_text(encoding="utf-8")
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
                        jit = subprocess.run([str(original), "run", str(source)], cwd=work,
                                            capture_output=True, text=True, encoding="utf-8", timeout=30)
                        application = work / executable_name("app")
                        built = subprocess.run([str(compiler), "build", str(source), "-o", str(application)],
                                               cwd=work, env=environment, capture_output=True,
                                               text=True, encoding="utf-8", timeout=30)
                        if jit.returncode == 65:
                            self.assertEqual(built.returncode, 65, built.stderr)
                            self.assertEqual(built.stderr, jit.stderr)
                        else:
                            self.assertEqual(built.returncode, 0, built.stderr)
                            native = subprocess.run([str(application)], cwd=work, env=environment,
                                                    capture_output=True, text=True, encoding="utf-8", timeout=30)
                            self.assertEqual(native.returncode, jit.returncode, native.stderr + jit.stderr)
                            self.assertEqual(native.stdout, jit.stdout)
                            self.assertEqual(built.stderr + native.stderr, jit.stderr)
                        source.unlink()
                        count += 1
                        compared += 1
                print(f"AOT/JIT corpus: {category}: {count} cases", flush=True)
            if not selected:
                self.assertGreater(compared, 350)

    def test_packaged_builds_and_source_free_execution(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vela-发行包 ") as temporary:
            work = Path(temporary)
            original = vela_executable()
            output = original.parent
            package = work / "toolchain 中"
            roots = create_native_package(original, package)
            environment = {key: value for key, value in os.environ.items()
                           if key.upper() not in {"PATH", "INCLUDE", "LIB", "LIBPATH", "VCTOOLSINSTALLDIR", "WINDOWSSDKDIR"}}
            environment.update(roots)
            environment["PATH"] = ""
            compiler = package / "bin" / executable_name("vela")

            def run(arguments: list[str | Path], expected: int = 0,
                    env: dict[str, str] | None = environment) -> subprocess.CompletedProcess:
                return native_command(self, arguments, expected, env, cwd=work)

            applications = []
            sources = []
            for name, text, expected, stdout in PROGRAM_CASES:
                with self.subTest(name=name):
                    source = work / (name + " 中.vela")
                    source.write_text(text + "\n", encoding="utf-8", newline="\n")
                    sources.append(source)
                    jit = run([original, "run", source], expected, None)
                    self.assertEqual(jit.stdout, stdout)
                    app = work / executable_name(name + " 原生")
                    run([compiler, "build", source, "-o", app])
                    native = run([app], expected)
                    self.assertEqual(native.stdout, jit.stdout)
                    self.assertEqual(native.stderr, jit.stderr)
                    applications.append((app, expected, native.stdout, native.stderr))

            pending_source = work / "pending.vela"
            pending_source.write_text(PENDING_SOURCE, encoding="utf-8", newline="\n")
            pending_app = work / executable_name("pending")
            run([compiler, "build", pending_source, "-o", pending_app])
            pending_source.unlink()
            pending_command(self, [pending_app], PENDING_OUTPUT_PREFIX, cwd=work, env=environment)

            broken_source = work / "closed_output.vela"
            broken_source.write_text(OUTPUT_FAILURE_SOURCE, encoding="utf-8", newline="\n")
            broken_app = work / executable_name("closed_output")
            run([compiler, "build", broken_source, "-o", broken_app])
            broken_source.unlink()
            self.assertIn("failed to write program output",
                          closed_output_command(self, [broken_app], cwd=work, env=environment))

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
            checked_manifest = Path(str(libraries[0]) + ".aot.json")
            original_manifest = checked_manifest.read_text(encoding="utf-8")
            for field, value, diagnostic in [
                ("format_version", 2, "identity mismatch"),
                ("target", "wrong", "identity mismatch"),
                ("runtime_abi", "wrong", "identity mismatch"),
                ("toolchain_build", "wrong", "identity mismatch"),
                ("optimization", "none", "unsupported native library optimization settings"),
            ]:
                with self.subTest(manifest_field=field):
                    checked_manifest.write_text(json.dumps(dict(manifests[0], **{field: value})),
                                                encoding="utf-8", newline="\n")
                    rejected = run([compiler, "build", "--verify-manifest=" + str(checked_manifest)], 65)
                    self.assertIn(diagnostic, rejected.stderr)
            checked_manifest.write_text(original_manifest, encoding="utf-8", newline="\n")
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
  auto isolate = std::unique_ptr<VelaAotIsolate, decltype(&vela_aot_v2_isolate_release)>(
      vela_aot_v2_isolate_create(), vela_aot_v2_isolate_release);
  ASSERT_NE(isolate, nullptr);
  vela::HandleScope scope(*isolate);
  auto numbers = NUMBERS::Instantiate(isolate.get());
  auto strings = STRINGS::Instantiate(isolate.get());
  ASSERT_TRUE(numbers.has_value());
  ASSERT_TRUE(strings.has_value());
  const auto scalar = NUMBERS::Call_calculate(scope, numbers->get(), 40);
  ASSERT_TRUE(scalar.has_value());
  EXPECT_EQ(*scalar, 42);
  auto input = vela::CreateString(scope, "你好");
  ASSERT_TRUE(input.has_value());
  auto text = STRINGS::Call_calculate(scope, strings->get(), *input);
  ASSERT_TRUE(text.has_value());
  strings->reset();
  EXPECT_EQ(text->Utf8(), "你好!");
}
'''.replace("NUMBERS_HEADER", libraries[0].name + ".h")
                .replace("STRINGS_HEADER", libraries[1].name + ".h")
                .replace("NUMBERS", namespaces[0]).replace("STRINGS", namespaces[1]), encoding="utf-8", newline="\n")
            host = work / executable_name("embedding")
            runtime = package / json.loads((package / "share/vela/target.json").read_text())["runtime"]
            test_libraries = native_test_libraries(output, runtime)
            includes = [package / "include", ROOT / "third_party/googletest/googletest/include"]
            compile_native_host(self, [host_source], host, [*libraries, *test_libraries],
                                includes, cwd=work)
            self.assertIn("[  PASSED  ] 1 test.", run([host]).stdout)

            c_source = work / "embedding.c"
            c_source.write_text(c_host_source(libraries[1].name + ".h", manifests[1]),
                                encoding="utf-8", newline="\n")
            c_object = work / ("embedding_c.obj" if sys.platform == "win32" else "embedding_c.o")
            c_host = work / executable_name("embedding_c")
            if sys.platform == "win32":
                run([tools["cxx"], "/nologo", "/MT", "/TC", "/W4", "/WX", "/c", c_source,
                     "/I" + str(package / "include"), "/Fo" + str(c_object)], env=None)
            else:
                c_flags = ["-std=c17", "-Werror", "-c"]
                if sys.platform == "darwin":
                    c_flags += ["-isysroot", str(tools["sysroot"])]
                run([tools["cc"], *c_flags, "-I" + str(package / "include"), c_source,
                     "-o", c_object], env=None)
            compile_native_host(self, [], c_host, [c_object, libraries[1], runtime], [], cwd=work)
            self.assertEqual(run([c_host]).stdout, "C host passed\n")

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
            archive_only = {key: value for key, value in target.items()
                            if key not in {"link_arguments", "linker", "runtime"}}
            target_path.write_text(json.dumps(archive_only), encoding="utf-8")
            run([compiler, "build", sources[0], "--emit=staticlib", "-o",
                 work / "archive-only.lib"], env=without_sdk)
            executable_only = {key: value for key, value in target.items()
                               if key not in {"archive_arguments", "archiver"}}
            target_path.write_text(json.dumps(executable_only), encoding="utf-8")
            run([compiler, "build", sources[0], "-o", work / executable_name("link-only")])
            target_path.write_text(json.dumps(target), encoding="utf-8")
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
