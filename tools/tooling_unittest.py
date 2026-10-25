"""Exercise the shipped language server through its actual binary stdio transport."""

from __future__ import annotations

import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tools.config import ROOT, executable_name


def frame(message: dict) -> bytes:
    body = json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    return f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body


def request(method: str, params: dict | None = None, identity: int | str | None = None) -> dict:
    message = {"jsonrpc": "2.0", "method": method}
    if params is not None:
        message["params"] = params
    if identity is not None:
        message["id"] = identity
    return message


class LanguageServerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="vela-lsp-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.entry = self.directory / "main.vela"
        self.uri = self.entry.as_uri()
        self.executable = Path(os.environ.get(
            "VELA_TEST_EXECUTABLE", ROOT / "out" / "debug" / executable_name("vela")))

    def decode(self, wire: bytes) -> list[dict]:
        stream = io.BytesIO(wire)
        messages = []
        while stream.tell() < len(wire):
            header = stream.readline()
            self.assertTrue(header.startswith(b"Content-Length: "), header)
            self.assertTrue(header.endswith(b"\r\n"), header)
            size = int(header.split(b":", 1)[1].strip())
            self.assertEqual(stream.readline(), b"\r\n")
            body = stream.read(size)
            self.assertEqual(len(body), size)
            message = json.loads(body)
            self.assertEqual(message["jsonrpc"], "2.0")
            messages.append(message)
        return messages

    def run_wire(self, wire: bytes, expected_exit: int = 0) -> list[dict]:
        result = subprocess.run([str(self.executable), "lsp"], input=wire,
                                capture_output=True, cwd=ROOT, timeout=20, check=False)
        self.assertEqual(result.returncode, expected_exit, result.stderr.decode("utf-8"))
        return self.decode(result.stdout)

    def session(self, messages: list[dict], encoding: str = "utf-16") -> list[dict]:
        initialize = request("initialize", {"capabilities": {
            "general": {"positionEncodings": [encoding]}}}, 1)
        shutdown = request("shutdown", identity=999)
        return self.run_wire(b"".join(frame(item) for item in [
            initialize, request("initialized", {}), *messages, shutdown, request("exit")]))

    def open(self, text: str, uri: str | None = None, version: int = 1) -> dict:
        return request("textDocument/didOpen", {"textDocument": {
            "uri": uri or self.uri, "languageId": "vela", "version": version, "text": text}})

    def change(self, changes: list[dict], version: int = 2, uri: str | None = None) -> dict:
        return request("textDocument/didChange", {"textDocument": {
            "uri": uri or self.uri, "version": version}, "contentChanges": changes})

    def reply(self, messages: list[dict], identity: int | str) -> dict:
        replies = [message for message in messages if message.get("id") == identity]
        self.assertEqual(len(replies), 1, messages)
        return replies[0]

    def diagnostics(self, messages: list[dict], uri: str | None = None) -> list[dict]:
        return [message["params"] for message in messages
                if message.get("method") == "textDocument/publishDiagnostics"
                and message["params"]["uri"] == (uri or self.uri)]

    def test_lifecycle_and_encoding_negotiation(self) -> None:
        for encoding in ("utf-8", "utf-16"):
            with self.subTest(encoding=encoding):
                replies = self.session([], encoding)
                capabilities = self.reply(replies, 1)["result"]["capabilities"]
                self.assertEqual(capabilities["positionEncoding"], encoding)
                self.assertEqual(capabilities["textDocumentSync"]["change"], 2)
                self.assertTrue(capabilities["documentFormattingProvider"])
                self.assertTrue(capabilities["definitionProvider"])
                self.assertTrue(capabilities["hoverProvider"])
                self.assertEqual(capabilities['documentLinkProvider'], {'resolveProvider': False})
                self.assertEqual(capabilities["completionProvider"]["triggerCharacters"], ["."])
                self.assertIsNone(self.reply(replies, 999)["result"])

    def test_hover_signatures_inferred_bindings_and_documentation(self) -> None:
        source = ("/// Adds two values.\n/// Evaluates each argument once.\n"
                  "func add(left: int, right: int): int { return left + right; }\n"
                  "const base = 40;\n"
                  "func main(): int { const result = add(base, 2); return result; }")
        queries = [(source.index("add(base"), "func add(left: int, right: int): int"),
                   (source.index("left +"), "left: int"),
                   (source.index("base, 2"), "const base: int"),
                   (source.index("return result") + 7, "const result: int")]
        messages = self.session([self.open(source), *[
            self.symbol_query("hover", source, offset, str(index))
            for index, (offset, _) in enumerate(queries)]])
        for index, (offset, signature) in enumerate(queries):
            hover = self.reply(messages, str(index))["result"]
            self.assertEqual(hover["contents"]["kind"], "markdown")
            self.assertTrue(hover["contents"]["value"].startswith(f"```vela\n{signature}\n```"), hover)
            prefix = source[:offset].rsplit("\n", 1)[-1]
            self.assertEqual(hover["range"]["start"]["character"], len(prefix))
        self.assertIn("Adds two values.\nEvaluates each argument once.",
                      self.reply(messages, "0")["result"]["contents"]["value"])

    def test_hover_members_types_aliases_and_builtins(self) -> None:
        source = ("interface Readable { read(): int; }\n"
                  "enum Mode { light, dark }\n"
                  "type Count = int;\n"
                  "class Counter {\n  /// Current count.\n  public param value: int;\n"
                  "  read(): int { return this.value; }\n}\n"
                  "func main(): void { const counter = Counter(value: 42);\n"
                  "const mode: Mode = Mode.light; const count: Count = counter.read();\n"
                  "const text = 'hello'; const values = Set<int>();\n"
                  "print(text.slice(0, 1)); print(text.length); print(values.has(count)); }")
        queries = [("Counter(value", "class Counter"),
                   ("value: 42", "value: int"),
                   ("Mode.light", "enum Mode"),
                   ("light;", "light: Mode"),
                   ("Count = counter", "type Count = int"),
                   ("read();", "func read(): int"),
                   ("slice(0", "func slice(start: int, end: int): string"),
                   ("length)", "length: int"),
                   ("has(count", "func has(value: int): bool"),
                   ("print(text", "func print(value): void"),
                   ("Readable", "interface Readable")]
        messages = self.session([self.open(source), *[
            self.symbol_query("hover", source, source.index(needle), str(index))
            for index, (needle, _) in enumerate(queries)]])
        self.assertEqual(self.diagnostics(messages)[-1]["diagnostics"], [])
        for index, (_, signature) in enumerate(queries):
            hover = self.reply(messages, str(index))["result"]
            self.assertIsNotNone(hover, signature)
            self.assertIn(f"```vela\n{signature}\n```", hover["contents"]["value"])
        self.assertIn("Current count.", self.reply(messages, "1")["result"]["contents"]["value"])

    def test_standard_globals_and_collection_methods_complete_and_hover_without_imports(self) -> None:
        source = ("func main(): void { const values = [3, 1, 2]; const keys = Set<int>();\n"
                  "values.sort((a: int, b: int): int => a - b);\n"
                  "print(Math.sqrt(9.0)); print(keys.union(keys).length); }")
        messages = self.session([self.open(source),
            self.symbol_query('completion', source, source.index('values.sort') + len('values.'), 'array'),
            self.symbol_query('completion', source, source.index('Math.sqrt') + len('Math.'), 'math'),
            self.symbol_query('completion', source, source.index('keys.union') + len('keys.'), 'set'),
            self.symbol_query('hover', source, source.index('sort('), 'sort'),
            self.symbol_query('hover', source, source.index('sqrt('), 'sqrt')])
        self.assertEqual(self.diagnostics(messages)[-1]['diagnostics'], [])
        self.assertIn('sort', {item['label'] for item in self.reply(messages, 'array')['result']})
        self.assertIn('sqrt', {item['label'] for item in self.reply(messages, 'math')['result']})
        self.assertIn('union', {item['label'] for item in self.reply(messages, 'set')['result']})
        self.assertIn('func sort(compare: (int, int) => int): void', self.reply(messages, 'sort')['result']['contents']['value'])
        self.assertIn('func sqrt(value: double): double', self.reply(messages, 'sqrt')['result']['contents']['value'])

    def test_array_join_completion_and_hover_replace_global_builtin(self) -> None:
        source = ("func main(): void { const words = ['a', 'b']; const numbers = [1, 2]; "
                  "const nested: int[][] = [[1]]; "
                  "print(words.join(',')); print(numbers.join(',')); print(nested.length); }")
        queries = [self.open(source)]
        for name in ('words', 'numbers', 'nested'):
            queries.append(self.symbol_query('completion', source,
                source.index(name + '.') + len(name) + 1, name))
        queries.extend([
            self.symbol_query('completion', source, source.index('print(words'), 'global'),
            self.symbol_query('hover', source, source.index('join('), 'hover')])
        messages = self.session(queries)
        self.assertEqual(self.diagnostics(messages)[-1]['diagnostics'], [])
        for name in ('words', 'numbers'):
            self.assertIn('join', {item['label'] for item in self.reply(messages, name)['result']})
        self.assertNotIn('join', {item['label'] for item in self.reply(messages, 'global')['result']})
        self.assertNotIn('join', {item['label'] for item in self.reply(messages, 'nested')['result']})
        self.assertIn('func join(separator: string): string',
                      self.reply(messages, 'hover')['result']['contents']['value'])

    def test_hover_uses_unsaved_imports_and_invalidates_cached_snapshot(self) -> None:
        library = self.directory / "library.vela"
        library.write_text("export func answer(): int { return 42; }", encoding="utf-8", newline="\n")
        source = "import 'library.vela' as lib; func main(): void { print(lib.answer()); }"
        offset = source.index("answer()")
        changed = "/// Unsaved answer.\nexport func answer(): string { return 'yes'; }"
        messages = self.session([
            self.open(source), self.symbol_query("hover", source, offset, "disk"),
            self.open(changed, library.as_uri()),
            self.symbol_query("hover", source, offset, "overlay"),
            request("textDocument/didClose", {"textDocument": {"uri": library.as_uri()}}),
            self.symbol_query("hover", source, offset, "closed")])
        self.assertIn("func answer(): int", self.reply(messages, "disk")["result"]["contents"]["value"])
        overlay = self.reply(messages, "overlay")["result"]["contents"]["value"]
        self.assertIn("func answer(): string", overlay)
        self.assertIn("Unsaved answer.", overlay)
        self.assertNotIn("$module", overlay)
        self.assertEqual(self.reply(messages, "closed")["result"], self.reply(messages, "disk")["result"])

    def test_hover_generic_functions_nullable_arrays_closures_and_standard_environment(self) -> None:
        source = ("/// Returns the input.\nfunc identity<T>(value: T): T { return value; }\n"
                  "func main(): void { let values: int?[] = [42, null];\n"
                  "const operation = (value: int): int => value + 1;\n"
                  "print(values.length); print(operation(identity<int>(42))); print(Math.sqrt(9.0)); }")
        queries = [("values.length", "values: int?[]"),
                   ("operation(identity", "const operation: (int) => int"),
                   ("identity<int>", "func identity<T>(value: T): T"),
                   ("sqrt(9.0)", "func sqrt(")]
        messages = self.session([self.open(source), *[
            self.symbol_query("hover", source, source.index(needle), str(index))
            for index, (needle, _) in enumerate(queries)]])
        self.assertEqual(self.diagnostics(messages)[-1]["diagnostics"], [])
        for index, (_, signature) in enumerate(queries):
            hover = self.reply(messages, str(index))["result"]
            self.assertIsNotNone(hover, signature)
            self.assertIn(signature, hover["contents"]["value"])
        self.assertIn("Returns the input.", self.reply(messages, "2")["result"]["contents"]["value"])

    def test_hover_ranges_use_negotiated_encoding_and_identifier_boundaries(self) -> None:
        source = "func main(): void { const text = '中😀'; print(text); }"
        offset = source.index("text);")
        for encoding in ("utf-8", "utf-16"):
            messages = self.session([self.open(source), *[
                self.symbol_query("hover", source, cursor, str(index), encoding=encoding)
                for index, cursor in enumerate((offset, offset + 4))]], encoding)
            hover = self.reply(messages, "0")["result"]
            self.assertEqual(hover, self.reply(messages, "1")["result"])
            prefix = source[:offset]
            expected = len(prefix.encode("utf-8")) if encoding == "utf-8" else len(prefix.encode("utf-16-le")) // 2
            self.assertEqual(hover["range"]["start"]["character"], expected)
            self.assertEqual(hover["range"]["end"]["character"], expected + 4)

    def test_hover_has_no_result_for_comments_literals_unknown_names_or_whitespace(self) -> None:
        source = "func main(): void {\n  // print and main\n  print('main');\n  missing;\n}\n"
        offsets = [source.index("print and"), source.index("'main'") + 2,
                   source.index("missing"), source.index("  print"), len(source)]
        messages = self.session([self.open(source), *[
            self.symbol_query("hover", source, offset, str(index))
            for index, offset in enumerate(offsets)]])
        for index in range(len(offsets)):
            self.assertIsNone(self.reply(messages, str(index))["result"])

    def test_checks_unsaved_text_and_clears_errors_at_new_version(self) -> None:
        messages = self.session([
            self.open("func main(): int { return true; }"),
            self.change([{"text": "func main(): int { return 42; }"}]),
        ])
        diagnostics = self.diagnostics(messages)
        self.assertTrue(diagnostics[0]["diagnostics"])
        self.assertEqual(diagnostics[0]["version"], 1)
        self.assertEqual(diagnostics[-1]["version"], 2)
        self.assertEqual(diagnostics[-1]["diagnostics"], [])
        self.assertFalse(self.entry.exists())

    def test_maps_diagnostic_columns_after_chinese_and_emoji(self) -> None:
        source = 'func main(): int { const text = "中😀"; return true; }'
        for encoding in ("utf-8", "utf-16"):
            with self.subTest(encoding=encoding):
                messages = self.session([self.open(source)], encoding)
                diagnostic = self.diagnostics(messages)[-1]["diagnostics"][0]
                # CheckReturn diagnoses the return statement's source location.
                prefix = source[:source.index("return")]
                expected = len(prefix.encode("utf-8")) if encoding == "utf-8" else len(prefix.encode("utf-16-le")) // 2
                self.assertEqual(diagnostic["range"]["start"], {"line": 0, "character": expected})

    def test_incremental_edits_use_negotiated_units_and_stale_versions_are_ignored(self) -> None:
        source = 'func main(): int { const text = "😀"; return true; }'
        prefix = source[:source.index("true")]
        start = len(prefix.encode("utf-16-le")) // 2
        edit = {"range": {"start": {"line": 0, "character": start},
                          "end": {"line": 0, "character": start + 4}}, "text": "42"}
        messages = self.session([
            self.open(source), self.change([edit], 3),
            self.change([{"text": source}], 2),
            request("textDocument/formatting", {"textDocument": {"uri": self.uri}, "options": {}}, "format"),
        ])
        self.assertEqual(self.diagnostics(messages)[-1]["version"], 3)
        self.assertEqual(self.diagnostics(messages)[-1]["diagnostics"], [])
        text = self.reply(messages, "format")["result"][0]["newText"]
        self.assertIn("return 42;", text)

    def test_invalid_multi_edit_is_transactional(self) -> None:
        source = 'func main(): int { const text = "😀"; return 42; }'
        prefix = source[:source.index("😀")]
        inside_surrogate = len(prefix.encode("utf-16-le")) // 2 + 1
        bad = {"range": {"start": {"line": 0, "character": inside_surrogate},
                         "end": {"line": 0, "character": inside_surrogate}}, "text": "x"}
        messages = self.session([
            self.open(source), self.change([{"text": source.replace("42", "43")}, bad]),
            request("textDocument/formatting", {"textDocument": {"uri": self.uri}}, 2),
        ])
        text = self.reply(messages, 2)["result"][0]["newText"]
        self.assertIn("return 42;", text)
        logs = [m for m in messages if m.get("method") == "window/logMessage"]
        self.assertEqual(len(logs), 1)
        self.assertIn("Unicode", logs[0]["params"]["message"])

    def test_imports_use_all_open_overlays_and_close_restores_disk_input(self) -> None:
        library = self.directory / "library.vela"
        library.write_text("export func answer(): bool { return true; }", encoding="utf-8", newline="\n")
        root = 'import "library.vela"; func main(): int { return answer(); }'
        messages = self.session([
            self.open(root), self.open("export func answer(): int { return 42; }", library.as_uri()),
            request("textDocument/didClose", {"textDocument": {"uri": library.as_uri()}}),
        ])
        diagnostics = self.diagnostics(messages)
        self.assertTrue(diagnostics[0]["diagnostics"])
        self.assertEqual(diagnostics[1]["diagnostics"], [])
        self.assertTrue(diagnostics[-1]["diagnostics"])
        self.assertEqual(self.diagnostics(messages, library.as_uri())[-1]["diagnostics"], [])

    def test_formats_without_rewriting_or_executing_the_document(self) -> None:
        source = 'func main():int{print("must not run");return 42;}'
        messages = self.session([
            self.open(source), request("textDocument/formatting", {"textDocument": {"uri": self.uri}}, 2),
        ])
        edits = self.reply(messages, 2)["result"]
        self.assertEqual(len(edits), 1)
        self.assertEqual(edits[0]["newText"], "func main(): int {\n  print('must not run');\n  return 42;\n}\n")
        self.assertFalse(self.entry.exists())

    def test_clears_diagnostics_for_dependencies_removed_from_the_graph(self) -> None:
        library = self.directory / "bad.vela"
        library.write_text("export func bad(): int { return true; }", encoding="utf-8", newline="\n")
        messages = self.session([
            self.open('import "bad.vela"; func main(): int { return 42; }'),
            self.change([{"text": "func main(): int { return 42; }"}]),
        ])
        diagnostics = self.diagnostics(messages, library.as_uri())
        self.assertTrue(diagnostics[0]["diagnostics"])
        self.assertEqual(diagnostics[-1]["diagnostics"], [])

    def test_reports_syntax_errors_in_unopened_import_using_its_loaded_snapshot(self) -> None:
        library = self.directory / "bad.vela"
        library.write_text("export func bad(\n", encoding="utf-8", newline="\n")
        messages = self.session([self.open('import "bad.vela"; func main(): int { return 42; }')])
        diagnostics = self.diagnostics(messages, library.as_uri())
        self.assertTrue(diagnostics[-1]["diagnostics"])
        self.assertEqual(diagnostics[-1]["diagnostics"][0]["range"]["start"]["line"], 1)

    def test_unknown_requests_and_invalid_format_params_are_explicit_errors(self) -> None:
        messages = self.session([request("unknown", {}, "unknown"), request("textDocument/formatting", {}, "bad")])
        self.assertEqual(self.reply(messages, "unknown")["error"]["code"], -32601)
        self.assertEqual(self.reply(messages, "bad")["error"]["code"], -32602)

    def test_recovers_after_invalid_json_and_enforces_initialization(self) -> None:
        wire = b"Content-Length: 1\r\n\r\n{" + frame(request("shutdown", identity=4))
        wire += frame(request("initialize", {"capabilities": {}}, 1))
        wire += frame(request("shutdown", identity=2)) + frame(request("exit"))
        messages = self.run_wire(wire)
        self.assertEqual(self.reply(messages, None)["error"]["code"], -32700)
        self.assertEqual(self.reply(messages, 4)["error"]["code"], -32002)
        self.assertEqual(self.reply(messages, 1)["result"]["capabilities"]["positionEncoding"], "utf-16")

    def test_exit_without_shutdown_is_unsuccessful(self) -> None:
        self.run_wire(frame(request("exit")), expected_exit=1)

    def test_formatting_invalid_source_returns_request_failed(self) -> None:
        messages = self.session([
            self.open("func main("), request("textDocument/formatting", {"textDocument": {"uri": self.uri}}, 2),
        ])
        self.assertEqual(self.reply(messages, 2)["error"]["code"], -32803)

    def symbol_query(self, method: str, source: str, offset: int,
                     identity: str = "query", encoding: str = "utf-16") -> dict:
        prefix = source[:offset]
        column = prefix.rsplit("\n", 1)[-1]
        character = (len(column.encode("utf-8")) if encoding == "utf-8"
                     else len(column.encode("utf-16-le")) // 2)
        return request("textDocument/" + method, {"textDocument": {"uri": self.uri},
            "position": {"line": prefix.count("\n"), "character": character}}, identity)

    def test_completes_visible_names_and_replaces_the_whole_identifier(self) -> None:
        source = ("func answer(): int { return 42; }\n"
                  "func main(seed: int): int {\n"
                  "  const before = seed;\n"
                  "  { const hidden = 1; }\n"
                  "  befor;\n"
                  "  const after = 2;\n"
                  "  return before;\n}")
        offset = source.index("befor;") + 3
        messages = self.session([self.open(source), self.symbol_query("completion", source, offset)])
        items = self.reply(messages, "query")["result"]
        self.assertEqual([item["label"] for item in items], ["before"])
        self.assertEqual(items[0]["detail"], "int")
        self.assertEqual(items[0]["textEdit"], {"newText": "before", "range": {
            "start": {"line": 4, "character": 2}, "end": {"line": 4, "character": 7}}})
        messages = self.session([self.open(source),
            self.symbol_query("completion", source, source.index("befor;"))])
        labels = {item["label"] for item in self.reply(messages, "query")["result"]}
        self.assertTrue({"before", "seed", "answer", "main", "return", "int"} <= labels)
        self.assertFalse({"hidden", "after"} & labels)

    def test_completes_incomplete_members_with_correct_visibility_and_storage(self) -> None:
        header = ("class Counter {\n  public param value: int;\n"
                  "  private secret: int = 1;\n  public static total: int = 0;\n"
                  "  increment(): int { return this.value + 1; }\n}\n")
        for receiver, expected in (("counter", {"value", "increment"}), ("Counter", {"total"})):
            with self.subTest(receiver=receiver):
                source = header + "func main(): void {\n  const counter = Counter(value: 1);\n  " + receiver + "."
                messages = self.session([self.open(source), self.symbol_query("completion", source, len(source))])
                labels = {item["label"] for item in self.reply(messages, "query")["result"]}
                self.assertEqual(labels, expected)
        source = header.replace("return this.value + 1;", "this.; return this.value + 1;")
        offset = source.index("this.;") + len("this.")
        messages = self.session([self.open(source), self.symbol_query("completion", source, offset)])
        labels = {item["label"] for item in self.reply(messages, "query")["result"]}
        self.assertEqual(labels, {"value", "secret", "increment"})

    def test_completes_builtin_members_and_respects_null_refinement(self) -> None:
        for declaration, expected in (
                ('const value = "text";', {"slice", "length", "byteLength", "contains", "graphemes"}),
                ("const value: int[] = [1];", {"copy", "slice", "fill", "reverse", "length"}),
                ("const value = Set<int>();", {"add", "has", "remove", "clear", "values", "length"}),
                ("const value = Map<string, int>();", {"get", "set", "keys", "values", "length"}),
                ("const value: Expected<int, string> = ok(42);", {"hasValue", "value", "error"}),
                ("const value = 1;", {"toString", "toInt", "toDouble"})):
            with self.subTest(declaration=declaration):
                source = "func main(): void { " + declaration + " value.; }"
                offset = source.index("value.;") + len("value.")
                messages = self.session([self.open(source), self.symbol_query("completion", source, offset)])
                labels = {item["label"] for item in self.reply(messages, "query")["result"]}
                self.assertTrue(expected <= labels, labels)
        for guard, expected in (("", set()), ("if (value != null)", {"length", "contains"})):
            source = 'func main(value: string?): void { ' + guard + ' { value.; } }'
            offset = source.index("value.;") + len("value.")
            messages = self.session([self.open(source), self.symbol_query("completion", source, offset)])
            labels = {item["label"] for item in self.reply(messages, "query")["result"]}
            self.assertTrue(expected <= labels)
            if not expected:
                self.assertEqual(labels, set())

    def test_definition_opens_import_paths_and_alias_declarations(self) -> None:
        library = self.directory / 'library 中😀.vela'
        library.write_text('export const answer = 42;', encoding='utf-8', newline='\n')
        (self.directory / 'nested').mkdir()
        paths = (library.stem, './' + library.stem, library.name, './' + library.name, 'nested/../' + library.name,
                 library.as_posix(), 'libra\\u0072y 中😀.vela')
        for quote in ("'", '"'):
            for path in paths:
                with self.subTest(quote=quote, path=path):
                    source = f'import {quote}{path}{quote} as module; func main(): int {{ return 42; }}'
                    start = source.index(quote)
                    offsets = [start, start + 1, start + len(path), source.index('module')]
                    messages = self.session([self.open(source), *[
                        self.symbol_query('definition', source, offset, str(index))
                        for index, offset in enumerate(offsets)]])
                    self.assertEqual(self.diagnostics(messages)[-1]['diagnostics'], [])
                    for index in range(len(offsets)):
                        target = self.reply(messages, str(index))['result']
                        self.assertIsNotNone(target)
                        self.assertEqual(target['uri'], library.as_uri())
                        self.assertEqual(target['range'], {
                            'start': {'line': 0, 'character': 0}, 'end': {'line': 0, 'character': 0}})

    def test_import_definition_tracks_unsaved_paths_and_unsaved_modules(self) -> None:
        first = self.directory / 'first.vela'
        first.write_text('export const value = 1;', encoding='utf-8', newline='\n')
        second = self.directory / 'second.vela'
        source = "import 'first.vela'; func main(): int { return value; }"
        changed = source.replace('first.vela', 'second.vela')
        offset = source.index('first.vela') + 2
        messages = self.session([
            self.open(source), self.symbol_query('definition', source, offset, 'initial'),
            self.open('export const value = 2;', second.as_uri()),
            self.change([{'text': changed}]),
            self.symbol_query('definition', changed, offset, 'changed'),
            request('textDocument/didClose', {'textDocument': {'uri': second.as_uri()}}),
            self.symbol_query('definition', changed, offset, 'closed')])
        self.assertEqual(self.reply(messages, 'initial')['result']['uri'], first.as_uri())
        self.assertEqual(self.reply(messages, 'changed')['result']['uri'], second.as_uri())
        self.assertIsNone(self.reply(messages, 'closed')['result'])
        self.assertFalse(second.exists())

    def test_document_links_cover_import_contents_and_use_resolved_files(self) -> None:
        library = self.directory / 'library 中😀.vela'
        library.write_text('export const value = 42;', encoding='utf-8', newline='\n')
        specifier = './libra\\u0072y 中😀'
        source = f"// 中😀\nimport '{specifier}' as lib;\nfunc main(): string {{ return 'library 中😀'; }}"
        for encoding in ('utf-8', 'utf-16'):
            messages = self.session([self.open(source), request('textDocument/documentLink',
                {'textDocument': {'uri': self.uri}}, 'links')], encoding)
            links = self.reply(messages, 'links')['result']
            self.assertEqual(len(links), 1)
            self.assertEqual(links[0]['target'], library.as_uri())
            units = len(specifier.encode('utf-8')) if encoding == 'utf-8' else len(specifier.encode('utf-16-le')) // 2
            self.assertEqual(links[0]['range'], {'start': {'line': 1, 'character': 8},
                                                'end': {'line': 1, 'character': 8 + units}})
            self.assertTrue(links[0]['tooltip'])

    def test_document_links_track_unsaved_import_changes_and_do_not_link_missing_files(self) -> None:
        target = self.directory / 'library.vela'
        source = "import 'library'; func main(): void {}"
        query = lambda identity: request('textDocument/documentLink', {'textDocument': {'uri': self.uri}}, identity)
        messages = self.session([self.open(source), query('missing'),
            self.open('export const value = 42;', target.as_uri()), query('overlay'),
            self.change([{'text': "import 'other'; func main(): void {}"}]), query('changed')])
        self.assertEqual(self.reply(messages, 'missing')['result'], [])
        self.assertEqual(self.reply(messages, 'overlay')['result'][0]['target'], target.as_uri())
        self.assertEqual(self.reply(messages, 'changed')['result'], [])

    def test_document_links_require_an_open_document(self) -> None:
        messages = self.session([
            request('textDocument/documentLink', {}, 'missing'),
            request('textDocument/documentLink', {'textDocument': {'uri': self.uri}}, 'closed')])
        for identity in ('missing', 'closed'):
            self.assertEqual(self.reply(messages, identity)['error']['code'], -32602)

    def test_import_definition_survives_target_syntax_errors_and_is_not_a_string_heuristic(self) -> None:
        library = self.directory / 'library.vela'
        library.write_text('export func broken(', encoding='utf-8', newline='\n')
        source = ("import 'library.vela';\n// library.vela\n"
                  "func main(): void { const text = 'library.vela'; }")
        offsets = [source.index('library.vela'), source.index('// library') + 3,
                   source.rindex('library.vela'), source.index('import')]
        messages = self.session([self.open(source), *[
            self.symbol_query('definition', source, offset, str(index))
            for index, offset in enumerate(offsets)]])
        self.assertEqual(self.reply(messages, '0')['result']['uri'], library.as_uri())
        for index in range(1, len(offsets)):
            self.assertIsNone(self.reply(messages, str(index))['result'])

    def test_definition_does_not_invent_disk_paths_for_missing_or_embedded_modules(self) -> None:
        for path in ('missing.vela', 'std:math'):
            source = f"import '{path}'; func main(): void {{}}"
            messages = self.session([self.open(source),
                                     self.symbol_query('definition', source, source.index(path))])
            self.assertIsNone(self.reply(messages, 'query')['result'])

    def test_definition_resolves_shadowed_locals_parameters_and_closure_captures(self) -> None:
        source = ("func main(seed: int): int {\n  const value = seed;\n"
                  "  { const value = 7; print(value); }\n"
                  "  const read = (): int => value;\n  return read();\n}")
        queries = [(source.index("= seed") + 2, 0, source.index("seed:")),
                   (source.index("print(value)") + 6, 2, len("  { const ")),
                   (source.index("=> value") + 3, 1, len("  const "))]
        messages = self.session([self.open(source), *[
            self.symbol_query("definition", source, offset, str(index))
            for index, (offset, _, _) in enumerate(queries)]])
        for index, (_, line, character) in enumerate(queries):
            target = self.reply(messages, str(index))["result"]
            self.assertEqual(target["uri"], self.uri)
            self.assertEqual(target["range"]["start"], {"line": line, "character": character})

    def test_definition_resolves_members_enums_and_type_annotations(self) -> None:
        source = ("enum Color { red, blue }\n"
                  "class Box { public param value: int; get(): int { return this.value; } }\n"
                  "func main(): int { const box: Box = Box(value: 4); print(Color.red); return box.get(); }")
        queries = [("this.value", "value", 1, source.splitlines()[1].index("value:")),
                   ("box.get", "get", 1, source.splitlines()[1].index("get()")),
                   ("Color.red", "red", 0, source.index("red,")),
                   ("box: Box", "Box", 1, len("class "))]
        messages = self.session([self.open(source), *[
            self.symbol_query("definition", source, source.index(fragment) + fragment.index(name), str(index))
            for index, (fragment, name, _, _) in enumerate(queries)]])
        for index, (_, name, line, character) in enumerate(queries):
            target = self.reply(messages, str(index))["result"]
            self.assertEqual(target["range"], {"start": {"line": line, "character": character},
                                               "end": {"line": line, "character": character + len(name)}})

    def test_import_queries_use_unsaved_dependencies_and_invalidate_cached_definitions(self) -> None:
        library = self.directory / "library.vela"
        library.write_text("export func answer(): int { return 1; }", encoding="utf-8")
        overlay = "func hidden(): int { return 0; }\nexport func answer(): int { return 42; }"
        source = 'import "library.vela" as lib;\nfunc main(): int { return lib.answer(); }'
        offset = source.index("answer();")
        messages = self.session([
            self.open(source), self.symbol_query("definition", source, offset, "disk"),
            self.open(overlay, library.as_uri()),
            self.symbol_query("completion", source, offset),
            self.symbol_query("definition", source, offset, "overlay"),
            self.change([{"text": "export func newer(): int { return 43; }"}], uri=library.as_uri()),
            self.symbol_query("completion", source, offset, "changed"),
            request("textDocument/didClose", {"textDocument": {"uri": library.as_uri()}}),
            self.symbol_query("definition", source, offset, "closed"),
        ])
        self.assertEqual([item["label"] for item in self.reply(messages, "query")["result"]], ["answer"])
        target = self.reply(messages, "overlay")["result"]
        self.assertEqual(target["uri"], library.as_uri())
        self.assertEqual(target["range"]["start"], {"line": 1, "character": len("export func ")})
        for identity in ("disk", "closed"):
            self.assertEqual(self.reply(messages, identity)["result"]["range"]["start"]["line"], 0)
        self.assertEqual([item["label"] for item in self.reply(messages, "changed")["result"]], ["newer"])

    def test_symbol_queries_negotiate_unicode_positions_and_validate_input(self) -> None:
        source = 'func main(): int { const text = "中😀"; const answer = 42; return answer; }'
        offset = source.index("answer;")
        for encoding in ("utf-8", "utf-16"):
            with self.subTest(encoding=encoding):
                messages = self.session([self.open(source),
                    self.symbol_query("definition", source, offset, encoding=encoding),
                    request("textDocument/completion", {"textDocument": {"uri": self.uri},
                        "position": {"line": 1000, "character": 0}}, "invalid")], encoding)
                target = self.reply(messages, "query")["result"]
                prefix = source[:source.index("answer =")]
                expected = len(prefix.encode("utf-8")) if encoding == "utf-8" else len(prefix.encode("utf-16-le")) // 2
                self.assertEqual(target["range"]["start"]["character"], expected)
                self.assertEqual(self.reply(messages, "invalid")["error"]["code"], -32602)

    def test_comments_and_string_text_have_no_symbol_completions(self) -> None:
        for source, fragment in [('func main(): void { const text = "words"; }', 'word'),
                                  ('func main(): void { /* comment */ }', 'comme'),
                                  ('func main(): void { // comment\n}', 'comme')]:
            with self.subTest(source=source):
                offset = source.index(fragment) + len(fragment)
                messages = self.session([self.open(source), self.symbol_query("completion", source, offset)])
                self.assertEqual(self.reply(messages, "query")["result"], [])

    def test_named_arguments_jump_to_parameters_and_constructor_fields(self) -> None:
        source = ("class Box { public param value: int; }\n"
                  "func add(left: int, right: int): int { return left + right; }\n"
                  "func main(): int { const box = Box(value: 1); return add(left: box.value, right: 2); }")
        queries = [(source.index("value: 1"), 0, source.index("value:")),
                   (source.index("left: box"), 1, len("func add(")),
                   (source.index("right: 2"), 1, source.splitlines()[1].index("right:"))]
        messages = self.session([self.open(source), *[
            self.symbol_query("definition", source, offset, str(index))
            for index, (offset, _, _) in enumerate(queries)]])
        for index, (_, line, character) in enumerate(queries):
            target = self.reply(messages, str(index))["result"]
            self.assertEqual(target["range"]["start"], {"line": line, "character": character})

    def test_for_catch_and_match_bindings_stay_in_their_scopes(self) -> None:
        source = ("class Failure { public param message: string; }\n"
                  "func main(): void {\n"
                  "  for (let index = 0; index < 2; index = index + 1) { print(index); }\n"
                  "  try { throw Failure(message: \"error\"); } catch (caught: Failure) { print(caught.message); }\n"
                  "  const result = match (true) { let matched => matched };\n"
                  "  print(result);\n}")
        messages = self.session([self.open(source),
            self.symbol_query("completion", source, source.index("print(result)")),
            self.symbol_query("definition", source, source.index("caught.message"), "catch"),
            self.symbol_query("definition", source, source.index("=> matched") + 3, "match")])
        labels = {item["label"] for item in self.reply(messages, "query")["result"]}
        self.assertNotIn("index", labels)
        self.assertNotIn("caught", labels)
        self.assertNotIn("matched", labels)
        self.assertIn("result", labels)
        self.assertEqual(self.reply(messages, "catch")["result"]["range"]["start"], {
            "line": 3, "character": source.splitlines()[3].index("caught:")})
        self.assertEqual(self.reply(messages, "match")["result"]["range"]["start"], {
            "line": 4, "character": source.splitlines()[4].index("matched =>")})

    def test_generic_alias_interface_and_chained_member_queries(self) -> None:
        source = ("interface Readable { read(): int; }\n"
                  "class Box<T> { public param value: T; }\n"
                  "type IntBox = Box<int>;\n"
                  "func main(reader: Readable): void { const box = IntBox(value: 42); box.value.; print(reader.read()); }")
        offset = source.index("box.value.;") + len("box.value.")
        messages = self.session([self.open(source), self.symbol_query("completion", source, offset)])
        labels = {item["label"] for item in self.reply(messages, "query")["result"]}
        self.assertTrue({"toString", "toInt", "toDouble"} <= labels)
        source = source.replace("box.value.;", "print(box.value);")
        messages = self.session([self.open(source),
            self.symbol_query("definition", source, source.index("box.value") + 4, "field"),
            self.symbol_query("definition", source, source.index("reader.read") + 7, "method")])
        self.assertEqual(self.reply(messages, "field")["result"]["range"]["start"]["line"], 1)
        self.assertEqual(self.reply(messages, "method")["result"]["range"]["start"]["line"], 0)

    def test_definition_accepts_identifier_end_before_punctuation_and_whitespace(self) -> None:
        source = ("class Box { public param value: int; }\n"
                  "func answer(): int { return 42; }\n"
                  "func main(): int { const box = Box(value: 1); print(box.value); return answer(); }")
        queries = [(source.index("answer();") + len("answer"), 1, len("func ")),
                   (source.index("box.value") + len("box"), 2, source.splitlines()[2].index("box =")),
                   (source.index("box.value") + len("box.value"), 0, source.index("value:"))]
        messages = self.session([self.open(source), *[
            self.symbol_query("definition", source, offset, str(index))
            for index, (offset, _, _) in enumerate(queries)]])
        for index, (_, line, character) in enumerate(queries):
            target = self.reply(messages, str(index))["result"]
            self.assertEqual(target["range"]["start"], {"line": line, "character": character})

    def test_exception_example_completes_and_navigates_while_a_statement_is_unfinished(self) -> None:
        source = (ROOT / "examples" / "exceptions.vela").read_text(encoding="utf-8")
        source = source.replace("    validate(-1);", "    va\n    validate(-1);")
        cursor = source.index("    va\n") + len("    va")
        call = source.index("validate(-1)") + len("validate")
        member = source.index("error.message") + len("error.")
        messages = self.session([self.open(source),
            self.symbol_query("completion", source, cursor),
            self.symbol_query("definition", source, call, "definition"),
            self.symbol_query("completion", source, member, "member")])
        self.assertEqual([item["label"] for item in self.reply(messages, "query")["result"]], ["validate"])
        self.assertEqual([item["label"] for item in self.reply(messages, "member")["result"]], ["message"])
        target = self.reply(messages, "definition")["result"]
        declaration = source[:source.index("func validate")].count("\n")
        self.assertEqual(target["range"]["start"], {"line": declaration, "character": len("func ")})
        # 编辑器恢复只保留可索引的 AST；真实编译仍必须拒绝这份缺分号的源码。
        self.assertTrue(self.diagnostics(messages)[-1]["diagnostics"])
        self.entry.write_text(source, encoding="utf-8", newline="\n")
        checked = subprocess.run([str(self.executable), "check", str(self.entry)],
                                 capture_output=True, cwd=ROOT, timeout=20, check=False)
        self.assertEqual(checked.returncode, 65, checked.stderr.decode("utf-8"))


if __name__ == "__main__":
    unittest.main()
