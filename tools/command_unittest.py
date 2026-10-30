"""Run source tools and the debugger through the shipped Vela executable."""

from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools.config import ROOT, executable_name


class CommandTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="vela-command-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.entry = self.directory / "main.vela"
        self.executable = Path(os.environ.get(
            "VELA_TEST_EXECUTABLE", ROOT / "out" / "debug" / executable_name("vela")))

    def write(self, source: str) -> None:
        self.entry.write_text(source, encoding="utf-8", newline="\n")

    def command(self, *arguments: str | Path, commands: str = "", expected: int = 0) -> subprocess.CompletedProcess:
        result = subprocess.run([str(self.executable), *map(str, arguments)], input=commands,
                                capture_output=True, text=True, encoding="utf-8",
                                cwd=self.directory, timeout=20, check=False)
        self.assertEqual(result.returncode, expected, result.stderr)
        return result

    def test_compiles_and_executes_an_image_after_sources_are_deleted(self) -> None:
        self.write("func main(): int { return 42; }")
        image = self.directory / "main.vbc"
        self.command("compile", self.entry, image)
        self.entry.unlink()
        self.command("exec", image, expected=42)
        data = bytearray(image.read_bytes())
        data[-1] ^= 1
        image.write_bytes(data)
        self.assertIn("checksum", self.command("exec", image, expected=65).stderr)

    def test_nonempty_switch_fallthrough_is_a_source_error(self) -> None:
        self.write("func main(): int { let value = 0; switch (1) { "
                   "case 1: value = 1; case 2: return 42; } return value; }")
        for arguments in [("check", self.entry), ("run", self.entry),
                          ("compile", self.entry, self.directory / "invalid.vbc")]:
            with self.subTest(command=arguments[0]):
                result = self.command(*arguments, expected=65)
                self.assertIn("error: non-empty switch branch must not fall through", result.stderr)
        self.assertFalse((self.directory / "invalid.vbc").exists())

    def test_dynamic_warnings_survive_cache_hits_and_project_errors_block_execution(self) -> None:
        self.write("func main():int { const value:dynamic=42; return value; }")
        for arguments in [("check", self.entry), ("run", self.entry), ("run", self.entry),
                          ("doc", self.entry, self.directory / "api.md")]:
            result = self.command(*arguments, expected=42 if arguments[0] == "run" else 0)
            self.assertIn("warning: [implicit_dynamic_conversion]", result.stderr)
        (self.directory / "vela.json").write_text(
            '{"diagnostics":{"implicit_dynamic_conversion":"error"}}',
            encoding="utf-8", newline="\n")
        for arguments in [("check", self.entry), ("run", self.entry),
                          ("compile", self.entry, self.directory / "rejected.vbc"),
                          ("doc", self.entry, self.directory / "rejected.md")]:
            result = self.command(*arguments, expected=65)
            self.assertIn("error: [implicit_dynamic_conversion]", result.stderr)
        self.assertFalse((self.directory / "rejected.vbc").exists())
        self.assertFalse((self.directory / "rejected.md").exists())

    def test_byte_views_survive_async_suspension_and_typed_containers(self) -> None:
        self.write("async func main(): Future<int> { "
                   "const data = ByteData.view(ByteArray(8)); data.setInt64(0,42); await 0; "
                   "const saved: ByteData[] = [data]; "
                   "for (let i = 0; i < 1000; i++) { const temporary = ByteArray(1024); } "
                   "return saved[0].getInt64(0); }")
        self.command("run", self.entry, expected=42)
        image = self.directory / "async-bytes.vbc"
        self.command("compile", self.entry, image)
        self.entry.unlink()
        self.command("exec", image, expected=42)

    def test_async_run_and_image_wait_for_background_timers(self) -> None:
        self.write("async func main(): Future<int> { "
                   "Timer.after(0, (): void => { print('timer'); }); return 42; }")
        self.assertEqual(self.command("run", self.entry, expected=42).stdout, "timer\n")
        image = self.directory / "async.vbc"
        self.command("compile", self.entry, image)
        self.entry.unlink()
        self.assertEqual(self.command("exec", image, expected=42).stdout, "timer\n")

    def test_full_case_mappings_match_pinned_ucd_inputs(self) -> None:
        data = (ROOT / "third_party/unicode_data/inputs/SpecialCasing.txt").read_text(encoding="utf-8")
        statements = []
        for line in data.splitlines():
            fields = [field.strip() for field in line.split("#", 1)[0].split(";")]
            if len(fields) < 5 or fields[4]:
                continue
            source = chr(int(fields[0], 16))
            for method, index in [("toLowerCase", 1), ("toUpperCase", 3)]:
                expected = "".join(chr(int(point, 16)) for point in fields[index].split())
                statements.append(f"if ('{source}'.{method}() != '{expected}') {{ return 1; }}")
        self.assertGreater(len(statements), 200)
        self.write("func main(): int {\n" + "\n".join(statements) + "\nreturn 0; }")
        self.command("run", self.entry)
        image = self.directory / "case.vbc"
        self.command("compile", self.entry, image)
        self.entry.unlink()
        self.command("exec", image)

    def test_byte_api_source_tools_and_source_free_image_execution(self) -> None:
        self.write("export func make(size: int): ByteData { return ByteData.view(ByteArray(size)); }\n"
                   "func main(): int { const view = make(4); view.setInt32(0,42); return view.getInt32(0); }")
        formatted = self.command("fmt", self.entry).stdout
        self.write(formatted)
        self.command("fmt", "--check", self.entry)
        output = self.directory / "api.md"
        self.command("doc", self.entry, output)
        self.assertIn("func make(size: int): ByteData", output.read_text(encoding="utf-8"))
        image = self.directory / "bytes.vbc"
        self.command("compile", self.entry, image)
        self.entry.unlink()
        self.command("exec", image, expected=42)

    def test_async_idle_and_background_errors_are_failures(self) -> None:
        self.write("func main(): Future<int> { return Future.race<int>([]); }")
        self.assertIn("idle/deadlock", self.command("run", self.entry, expected=70).stderr)
        self.write("func main(): int { Future.microtask((): void => { "
                   "throw Error(message: 'background'); }); return 0; }")
        self.assertIn("unhandled Future error: background", self.command("run", self.entry, expected=70).stderr)

    def test_async_failure_keeps_throw_and_await_locations(self) -> None:
        self.write("async func fail(): Future<int> { await 0; return 1 / 0; }\n"
                   "async func main(): Future<int> { return await fail(); }")
        result = self.command("run", self.entry, expected=70)
        self.assertIn("division by zero", result.stderr)
        self.assertIn("at fail", result.stderr)
        self.assertIn("at main", result.stderr)

    def test_documentation_preserves_async_and_function_value_types(self) -> None:
        self.write("export async func load(): Future<int> { return 42; }\n"
                   "export func bind(value: int): () => string { return value.toString; }")
        output = self.directory / "api.md"
        self.command("doc", self.entry, output)
        text = output.read_text(encoding="utf-8")
        self.assertIn("async func load(): Future<int>", text)
        self.assertIn("func bind(value: int): () => string", text)

    def test_json_dynamic_survive_documentation_formatting_images_and_repl(self) -> None:
        self.write("export func encode(value: dynamic): string { return Json.stringify(value); }\n"
                   "func main(): int { const root: dynamic = Json.parse('[40,true,null]'); "
                   "root[0] += 2; return encode(root) == '[42,true,null]' ? 42 : 0; }")
        formatted = self.command("fmt", self.entry).stdout
        self.write(formatted)
        self.command("fmt", "--check", self.entry)
        output = self.directory / "api.md"
        self.command("doc", self.entry, output)
        self.assertIn("func encode(value: dynamic): string", output.read_text(encoding="utf-8"))
        image = self.directory / "json.vbc"
        self.command("compile", self.entry, image)
        self.entry.unlink()
        self.command("exec", image, expected=42)
        result = self.command("repl", commands=(
            "const root: dynamic = Json.parse('[40,true,null]');\n"
            "root[0] += 2;\nJson.stringify(root)\n.quit\n"))
        self.assertIn("[42,true,null]", result.stdout)

    def test_source_entries_require_files_and_import_schemes_are_explicit(self) -> None:
        result = self.command("check", self.directory, expected=65)
        self.assertIn("source path must name a file", result.stderr)
        result = self.command("run", self.directory, expected=65)
        self.assertIn("source path must name a file", result.stderr)
        self.write('import "registry:math/value.vela";')
        result = self.command("check", self.entry, expected=65)
        self.assertIn("unsupported module import scheme", result.stderr)

    def test_repl_persists_multiline_functions_values_and_recovers_from_errors(self) -> None:
        commands = ('let x = 40;\nfunc add(value: int): int {\nreturn value + 2;\n}\n'
                    'add(x)\nlet bad: int = true;\nx = x + 1;\nx\nprint(1 / 0);\nx\n.quit\n')
        result = self.command("repl", commands=commands)
        self.assertEqual(result.stdout, "42\n41\n41\n")
        self.assertIn("... ", result.stderr)
        self.assertIn("does not match", result.stderr)
        self.assertIn("division by zero", result.stderr)

    def test_repl_commands_reset_and_eof(self) -> None:
        result = self.command("repl", commands="const x = 42;\n.source\n.reset\nx\n.help\n.quit\n")
        self.assertEqual(result.stdout, "const x = 42;\n\n")
        self.assertIn("<repl:1>", result.stderr)
        self.assertIn("unknown", result.stderr)
        self.assertIn("trailing semicolon", result.stderr)
        self.command("repl", commands="func unfinished(): void {\n", expected=65)
        self.command("repl", commands="")

    def test_repl_imports_resolve_in_working_directory(self) -> None:
        (self.directory / "math.vela").write_text(
            "export func answer(): int { return 42; }", encoding="utf-8")
        result = self.command("repl", commands='import "math.vela" as math;\nmath.answer()\n.quit\n')
        self.assertEqual(result.stdout, "42\n")

    def test_formatter_outputs_checks_and_writes_without_executing(self) -> None:
        self.write('func main():void{print("must not execute");}')
        formatted = self.command("fmt", self.entry).stdout
        self.assertEqual(formatted, "func main(): void {\n  print('must not execute');\n}\n")
        self.command("fmt", "--check", self.entry, expected=1)
        self.command("fmt", "--write", self.entry)
        self.assertEqual(self.entry.read_text(encoding="utf-8"), formatted)
        self.command("fmt", "--check", self.entry)
        self.write("func broken(")
        self.command("fmt", "--write", self.entry, expected=65)
        self.assertEqual(self.entry.read_text(encoding="utf-8"), "func broken(")

    def test_single_quote_formatting_preserves_execution_and_nested_interpolation(self) -> None:
        self.write(r'''class Item { text: string; }
func main(): void {
  print("it's \"quoted\" \u{1F600}");
  print("${Item(text: "${"nested"}").text}");
}''')
        before = self.command("run", self.entry).stdout
        self.assertEqual(before, 'it\'s "quoted" 😀\nnested\n')
        self.command("fmt", "--write", self.entry)
        formatted = self.entry.read_text(encoding="utf-8")
        self.assertIn("print('it\\'s", formatted)
        self.assertEqual(self.command("run", self.entry).stdout, before)
        self.command("fmt", "--check", self.entry)
        self.assertEqual(self.command("fmt", self.entry).stdout, formatted)

    def test_documentation_checks_modules_without_evaluating_initializers(self) -> None:
        self.write('/// A value.\nexport let value = fail();\n'
                   'func fail(): int { print("must not execute"); return 42; }\n')
        output = self.directory / "api.md"
        result = self.command("doc", self.entry, output)
        self.assertEqual(result.stdout, "")
        original = output.read_bytes()
        self.assertIn("A value.", original.decode("utf-8"))
        self.assertNotIn("## `fail`", original.decode("utf-8"))
        self.write("export let value: int = true;")
        self.command("doc", self.entry, output, expected=65)
        self.assertEqual(output.read_bytes(), original)

    def test_documentation_preserves_import_prefixes_and_unspecialized_generics(self) -> None:
        (self.directory / "library.vela").write_text(
            "/// Dependency export.\nexport class Box<T> { value: T; }\n", encoding="utf-8")
        self.write("import 'library.vela' as api;\n"
                   "/// Root generic export.\n"
                   "export func wrap<T>(value: T): api.Box<T> { return api.Box<T>(value: value); }\n"
                   "export func answer(): api.Box<int> { return wrap<int>(42); }\n")
        output = self.directory / "api.md"
        self.command("doc", self.entry, output)
        text = output.read_text(encoding="utf-8")
        self.assertIn("Root generic export.", text)
        self.assertIn("func wrap<T>(value: T): api.Box<T>;", text)
        self.assertIn("func answer(): api.Box<int>;", text)
        self.assertNotIn("Dependency export.", text)
        self.assertNotIn("$module", text)

    def test_unicode_source_and_output_paths(self) -> None:
        self.entry = self.directory / "你好😀.vela"
        self.write("func main(): int { return 42; }")
        output = self.directory / "镜像😀.vbc"
        self.command("check", self.entry)
        self.command("compile", self.entry, output)
        self.command("exec", output, expected=42)

    def test_function_breakpoint_stack_registers_and_finish(self) -> None:
        self.write("func child(value: int): int {\n  return value + 1;\n}\n"
                   "func main(): int {\n  let value = 40;\n  value = child(value);\n  return value + 1;\n}\n")
        result = self.command("debug", self.entry,
                              commands="break child\ncontinue\nstack\nregisters\nprint r0\nfinish\ncontinue\n", expected=42)
        self.assertIn("#0 child pc=", result.stderr)
        self.assertIn("#1 main pc=", result.stderr)
        self.assertIn("r0 = 40", result.stderr)
        self.assertIn("debug> 40", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_source_breakpoint_stops_once_per_loop_iteration(self) -> None:
        self.write("func work(): int {\n  let i = 0;\n  while (i < 3) {\n    i = i + 1;\n  }\n  return i;\n}\n"
                   "func main(): int { return work() + 39; }\n")
        result = self.command("debug", self.entry, commands="break 4\nc\nc\nc\nc\n", expected=42)
        self.assertEqual(len(re.findall(r"work pc=\d+ at [^\r\n]+:4:", result.stderr)), 3, result.stderr)

    def test_next_skips_a_call_but_step_enters_it(self) -> None:
        self.write("func child(value: int): int {\n  return value + 1;\n}\n"
                   "func main(): int {\n  let value = 40;\n  value = child(value);\n  return value + 1;\n}\n")
        skipped = self.command("debug", self.entry, commands="next\nquit\n")
        self.assertNotIn("child pc=", skipped.stderr)
        self.assertIn("debugger stopped", skipped.stderr)
        stepped = self.command("debug", self.entry, commands="step\nquit\n")
        self.assertIn("child pc=", stepped.stderr)

    def test_inspects_object_fields_and_array_elements(self) -> None:
        self.write("class Box { value: int; }\n"
                   "func inspect(box: Box, values: int[]): int { return box.value + values[0]; }\n"
                   "func main(): int { return inspect(Box(value: 40), [2]); }\n")
        result = self.command("debug", self.entry,
                              commands="break inspect\nc\nregisters\nfield r0 0\nelement r1 0\nc\n", expected=42)
        self.assertIn("r0 = object(fields=1)", result.stderr)
        self.assertIn("r1 = array(length=1)", result.stderr)
        self.assertIn("debug> 40", result.stderr)
        self.assertIn("debug> 2", result.stderr)

    def test_bad_debugger_commands_and_eof_never_execute_past_the_pause(self) -> None:
        self.write('func main(): void { print("must not execute"); }')
        result = self.command("debug", self.entry,
                              commands="print r999\nframe 99\ndelete 999\nbreak 999\nunknown\n")
        self.assertEqual(result.stdout, "")
        self.assertIn("register is out of range", result.stderr)
        self.assertIn("frame depth is out of range", result.stderr)
        self.assertIn("unknown breakpoint ID", result.stderr)
        self.assertIn("no executable line", result.stderr)
        self.assertIn("unknown debugger command", result.stderr)
        self.assertIn("debugger stopped", result.stderr)


if __name__ == "__main__":
    unittest.main()
