"""Exercise the shipped language server through its actual binary stdio transport."""

from __future__ import annotations

import io
import json
import subprocess
import unittest

from tools.config import ROOT
from tools.test_support import CommandTestCase


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


class LanguageServerTest(CommandTestCase):
    directory_prefix = "vela-lsp-"

    def setUp(self) -> None:
        super().setUp()
        self.entry = self.directory / "main.vela"
        self.uri = self.entry.as_uri()

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

    def check_hovers(self, source: str, checks: list[tuple[str, str]]) -> list[dict]:
        messages = self.session([self.open(source), *[
            self.symbol_query("hover", source, source.index(marker), str(index))
            for index, (marker, _) in enumerate(checks)]])
        self.assertEqual(self.diagnostics(messages)[-1]["diagnostics"], [])
        for index, (_, expected) in enumerate(checks):
            hover = self.reply(messages, str(index))["result"]
            self.assertIsNotNone(hover, expected)
            self.assertIn(expected, hover["contents"]["value"])
        return messages

    def check_definitions(self, source: str, queries: list[tuple[int, int, int]]) -> None:
        messages = self.session([self.open(source), *[
            self.symbol_query("definition", source, offset, str(index))
            for index, (offset, _, _) in enumerate(queries)]])
        for index, (_, line, character) in enumerate(queries):
            target = self.reply(messages, str(index))["result"]
            self.assertEqual(target["uri"], self.uri)
            self.assertEqual(target["range"]["start"], {"line": line, "character": character})

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
                self.assertEqual(capabilities["renameProvider"], {"prepareProvider": True})
                self.assertEqual(capabilities['documentLinkProvider'], {'resolveProvider': False})
                self.assertEqual(capabilities["completionProvider"]["triggerCharacters"], [".", "(", ","])
                self.assertNotIn("signatureHelpProvider", capabilities)
                self.assertEqual(capabilities['semanticTokensProvider']['legend'], {
                    'tokenTypes': ['class', 'interface', 'enum', 'type', 'typeParameter'],
                    'tokenModifiers': ['declaration', 'defaultLibrary']})
                self.assertTrue(capabilities['semanticTokensProvider']['full'])
                self.assertIsNone(self.reply(replies, 999)["result"])

    def test_hover_signatures_inferred_bindings_and_documentation(self) -> None:
        source = ("/// Adds two values.\n/// Evaluates each argument once.\n"
                  "func add(left: int, right: int): int { return left + right; }\n"
                  "const base = 40;\n"
                  "func main(): int { const result = add(base, 2); return result; }")
        queries = [(source.index("add(base"), "func add(left: int, right: int): int"),
                   (source.index("left +"), "(parameter) left: int"),
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

    def test_hover_uses_flow_types_at_each_reference(self) -> None:
        for binding, prefix in (("let", ""), ("const", "const ")):
            with self.subTest(binding=binding):
                source = ("func main(): void {\n"
                          "  /// The nullable value.\n"
                          f"  {binding} value: int? = 1;\n"
                          "  if (value != null) { print(value); }\n"
                          "  print(value);\n"
                          "}\n")
                declaration = source.index("value: int?")
                inside = source.index("print(value)") + len("print(")
                outside = source.rindex("print(value)") + len("print(")
                queries = ((declaration, "int?"), (inside, "int"), (outside, "int?"))
                messages = self.session([self.open(source), *[
                    self.symbol_query("hover", source, offset, str(index))
                    for index, (offset, _) in enumerate(queries)],
                    self.symbol_query("definition", source, inside, "definition")])
                self.assertEqual(self.diagnostics(messages)[-1]["diagnostics"], [])
                for index, (offset, expected) in enumerate(queries):
                    hover = self.reply(messages, str(index))["result"]
                    self.assertTrue(hover["contents"]["value"].startswith(
                        f"```vela\n{prefix}value: {expected}\n```"), hover)
                    self.assertIn("The nullable value.", hover["contents"]["value"])
                    line = source[:offset].count("\n")
                    character = len(source[:offset].rsplit("\n", 1)[-1])
                    self.assertEqual(hover["range"], {
                        "start": {"line": line, "character": character},
                        "end": {"line": line, "character": character + len("value")}})
                self.assertEqual(self.reply(messages, "definition")["result"]["range"]["start"],
                                 {"line": 2, "character": len(f"  {binding} ")})

    def test_hover_refines_parameters_after_early_returns(self) -> None:
        for condition in ("value == null", "!(value != null)"):
            with self.subTest(condition=condition):
                source = ("func main(value: int?): void {\n"
                          f"  if ({condition}) {{ return; }}\n"
                          "  print(value);\n"
                          "}\n")
                queries = ((source.index("value: int?"), "int?"),
                           (source.index(condition) + condition.index("value"), "int?"),
                           (source.index("print(value)") + len("print("), "int"))
                messages = self.session([self.open(source), *[
                    self.symbol_query("hover", source, offset, str(index))
                    for index, (offset, _) in enumerate(queries)]])
                self.assertEqual(self.diagnostics(messages)[-1]["diagnostics"], [])
                for index, (_, expected) in enumerate(queries):
                    hover = self.reply(messages, str(index))["result"]
                    self.assertEqual(hover["contents"]["value"],
                                     f"```vela\n(parameter) value: {expected}\n```")

    def test_hover_respects_reassignment_shadowing_and_capture_invalidation(self) -> None:
        cases = (
            ("func main(value: int?): void {\n"
             "  if (value == null) { return; }\n"
             "  print(value);\n"
             "  value = null;\n"
             "  print(value);\n"
             "}\n", ("int", "int?")),
            ("func main(value: int?): void {\n"
             "  if (value == null) { return; }\n"
             "  { let value: int? = null; print(value); }\n"
             "  print(value);\n"
             "}\n", ("int?", "int")),
            ("func main(value: int?): void {\n"
             "  const clear = (): void => { value = null; };\n"
             "  if (value == null) { return; }\n"
             "  print(value);\n"
             "  clear();\n"
             "  print(value);\n"
             "}\n", ("int", "int?")))
        for source, expected in cases:
            with self.subTest(source=source):
                offsets = (source.index("print(value)") + len("print("),
                           source.rindex("print(value)") + len("print("))
                messages = self.session([self.open(source), *[
                    self.symbol_query("hover", source, offset, str(index))
                    for index, offset in enumerate(offsets)]])
                self.assertEqual(self.diagnostics(messages)[-1]["diagnostics"], [])
                for index, type_name in enumerate(expected):
                    hover = self.reply(messages, str(index))["result"]
                    self.assertIn(f"value: {type_name}\n```", hover["contents"]["value"])

    def test_hover_uses_type_test_refinements_and_keeps_member_signatures(self) -> None:
        source = ("class Box {\n"
                  "  /// Stored value.\n"
                  "  value: int;\n"
                  "  read(): int { return this.value; }\n"
                  "}\n"
                  "func main(value: dynamic): void {\n"
                  "  if (!(value is Box)) { return; }\n"
                  "  print(value.value);\n"
                  "  print(value.read());\n"
                  "}\n")
        offset = source.index("value.value")
        queries = ((offset, "(parameter) value: Box"),
                   (offset + len("value."), "(property) Box.value: int"),
                   (source.index("read()", source.index("func main")), "(method) Box.read(): int"))
        messages = self.session([self.open(source), *[
            self.symbol_query("hover", source, cursor, str(index))
            for index, (cursor, _) in enumerate(queries)]])
        self.assertEqual(self.diagnostics(messages)[-1]["diagnostics"], [])
        for index, (_, signature) in enumerate(queries):
            self.assertTrue(self.reply(messages, str(index))["result"]["contents"]["value"].startswith(
                f"```vela\n{signature}\n```"))
        self.assertIn("Stored value.", self.reply(messages, "1")["result"]["contents"]["value"])

    def test_hover_keeps_named_argument_labels_distinct_from_narrowed_values(self) -> None:
        source = ("func accept(value: int?): void {}\n"
                  "func main(value: int?): void {\n"
                  "  if (value == null) { return; }\n"
                  "  accept(value: value);\n"
                  "}\n")
        label = source.rindex("accept(value:") + len("accept(")
        argument = label + len("value: ")
        messages = self.session([self.open(source),
            self.symbol_query("hover", source, label, "label"),
            self.symbol_query("hover", source, argument, "argument"),
            self.symbol_query("definition", source, label, "label-definition"),
            self.symbol_query("definition", source, argument, "argument-definition")])
        self.assertEqual(self.diagnostics(messages)[-1]["diagnostics"], [])
        for identity, type_name in (("label", "int?"), ("argument", "int")):
            self.assertEqual(self.reply(messages, identity)["result"]["contents"]["value"],
                             f"```vela\n(parameter) value: {type_name}\n```")
        for identity, line, character in (("label-definition", 0, len("func accept(")),
                                         ("argument-definition", 1, len("func main("))):
            self.assertEqual(self.reply(messages, identity)["result"]["range"]["start"],
                             {"line": line, "character": character})

    def test_json_and_checked_dynamic_share_types_hover_tokens_and_completion(self) -> None:
        source = ("func main(): void {\n"
                  "  const root: dynamic = Json.parse('[1,true,null]');\n"
                  "  const items: dynamic[] = root;\n"
                  "  print(Json.stringify(items));\n"
                  "}\n")
        messages = self.session([self.open(source),
            self.symbol_query("hover", source, source.index("dynamic"), "type"),
            self.symbol_query("hover", source, source.index("parse("), "parse"),
            self.symbol_query("hover", source, source.index("stringify("), "stringify"),
            self.symbol_query("hover", source, source.index("= root") + 2, "root"),
            self.symbol_query("completion", source, source.index("Json.parse") + len("Json."), "members"),
            self.symbol_query("completion", source, source.index("print("), "globals"),
            request("textDocument/semanticTokens/full", {"textDocument": {"uri": self.uri}}, "tokens")])
        diagnostics = self.diagnostics(messages)[-1]["diagnostics"]
        self.assertEqual(len(diagnostics), 1)
        self.assertEqual(diagnostics[0]["code"], "implicit_dynamic_conversion")
        self.assertEqual(diagnostics[0]["severity"], 2)
        for identity, signature in (("type", "type dynamic"),
                                    ("parse", "Json.parse(source: string): dynamic"),
                                    ("stringify", "Json.stringify(value: dynamic): string"),
                                    ("root", "const root: dynamic")):
            self.assertIn(signature, self.reply(messages, identity)["result"]["contents"]["value"])
        self.assertIn("checked at runtime", self.reply(messages, "type")["result"]["contents"]["value"])
        self.assertEqual({item["label"] for item in self.reply(messages, "members")["result"]},
                         {"parse", "stringify"})
        self.assertTrue({"Json", "dynamic"} <= {
            item["label"] for item in self.reply(messages, "globals")["result"]})
        tokens = self.semantic_tokens(messages)
        line = source.splitlines()[1]
        self.assertIn((1, line.index("dynamic"), 7, "type", 2), tokens)
        self.assertIn((1, line.index("Json"), 4, "class", 2), tokens)

    def test_dynamic_diagnostics_and_formatting_keep_explicit_type_boundaries(self) -> None:
        source = "func main():void{const values:dynamic[]=[1,true,null];const value:int=values[0];}"
        messages = self.session([self.open(source),
            request("textDocument/formatting", {"textDocument": {"uri": self.uri}}, "format"),
            self.change([{"text": "func main(): void { const value: dynamic? = null; }"}])])
        diagnostic = self.diagnostics(messages)[0]["diagnostics"]
        self.assertEqual(len(diagnostic), 1)
        self.assertEqual(diagnostic[0]["severity"], 2)
        self.assertEqual(diagnostic[0]["code"], "implicit_dynamic_conversion")
        self.assertTrue(any("already nullable" in item["message"]
                            for item in self.diagnostics(messages)[-1]["diagnostics"]))
        formatted = self.reply(messages, "format")["result"][0]["newText"]
        self.assertIn("const values: dynamic[] = [1, true, null];", formatted)
        self.assertIn("const value: int = values[0];", formatted)

    def test_project_severity_and_explicit_checks_share_compiler_diagnostics(self) -> None:
        (self.directory / "vela.json").write_text(
            json.dumps({"diagnostics": {"implicit_dynamic_conversion": "error"}}),
            encoding="utf-8", newline="\n")
        source = "func main():void { const root=Json.parse('{}'); const name:string=root.get('name') ?? ''; }"
        messages = self.session([self.open(source), self.change([{
            "text": source.replace("root.get('name') ?? ''", "(root.get('name') as string?) ?? ''")
        }])])
        diagnostics = self.diagnostics(messages)
        self.assertEqual(diagnostics[0]["diagnostics"][0]["severity"], 1)
        self.assertEqual(diagnostics[0]["diagnostics"][0]["code"], "implicit_dynamic_conversion")
        self.assertEqual(diagnostics[-1]["diagnostics"], [])

    def test_inferred_generic_calls_provide_hover_and_lambda_member_completion(self) -> None:
        source = ("class User { name:string; }\n"
                  "func apply<T,U>(operation:(T)=>U,value:T):U { return operation(value); }\n"
                  "func main():void { const name=apply(user=>user.name,User(name:'Vela')); }")
        line = source.splitlines()[2]
        messages = self.session([self.open(source),
            request("textDocument/hover", {"textDocument": {"uri": self.uri},
                "position": {"line": 2, "character": line.index("const name") + 7}}, "hover"),
            self.symbol_query("completion", source, source.index("user.name") + len("user."), "members")])
        self.assertEqual(self.diagnostics(messages)[-1]["diagnostics"], [])
        self.assertIn("string", str(self.reply(messages, "hover")["result"]))
        self.assertIn("name", {item["label"] for item in self.reply(messages, "members")["result"]})
        items = self.argument_completions(
            'class Box<T> { value: T; }\n'
            'func main(): void { const box: Box<int> = Box(¦); }')
        parameters = {item['label']: item['detail'] for item in items if item['label'].endswith(':')}
        self.assertEqual(parameters, {'value:': 'value: int'})

    def test_hover_members_types_aliases_and_builtins(self) -> None:
        source = ("interface Readable { read(): int; }\n"
                  "enum Mode { light, dark }\n"
                  "type Count = int;\n"
                  "class Counter {\n  /// Current count.\n  value: int;\n"
                  "  read(): int { return this.value; }\n}\n"
                  "func main(): void { const counter = Counter(value: 42);\n"
                  "const mode: Mode = Mode.light; const count: Count = counter.read();\n"
                  "const text = 'hello'; const values = Set<int>();\n"
                  "print(text.slice(0, 1)); print(text.length); print(values.has(count)); }")
        queries = [("Counter(value", "class Counter {\n  constructor(value: int);\n}"),
                   ("value: 42", "(property) Counter.value: int"),
                   ("Mode.light", "enum Mode"),
                   ("light;", "(enum member) Mode.light = 0"),
                   ("Count = counter", "type Count = int"),
                   ("read();", "(method) Counter.read(): int"),
                   ("slice(0", "(method) string.slice(start: int, end: int): string"),
                   ("length)", "(property) string.length: int"),
                   ("has(count", "(method) Set<int>.has(value: int): bool"),
                   ("print(text", "func print(value): void"),
                   ("Readable", "interface Readable")]
        messages = self.check_hovers(source, [(marker, f"```vela\n{signature}\n```")
                                              for marker, signature in queries])
        self.assertIn("Current count.", self.reply(messages, "1")["result"]["contents"]["value"])

    def test_standard_globals_and_collection_methods_complete_and_hover_without_imports(self) -> None:
        source = ("func main(): void { const values = [3, 1, 2]; const keys = Set<int>();\n"
                  "values.sort((a: int, b: int): int => a - b);\n"
                  "print(Math.sqrt(9.0)); print(keys.union(keys).size); }")
        messages = self.session([self.open(source),
            self.symbol_query('completion', source, source.index('values.sort') + len('values.'), 'array'),
            self.symbol_query('completion', source, source.index('Math.sqrt') + len('Math.'), 'math'),
            self.symbol_query('completion', source, source.index('print(Math.sqrt'), 'global'),
            self.symbol_query('completion', source, source.index('keys.union') + len('keys.'), 'set'),
            self.symbol_query('hover', source, source.index('sort('), 'sort'),
            self.symbol_query('hover', source, source.index('sqrt('), 'sqrt')])
        self.assertEqual(self.diagnostics(messages)[-1]['diagnostics'], [])
        self.assertIn('sort', {item['label'] for item in self.reply(messages, 'array')['result']})
        self.assertIn('sqrt', {item['label'] for item in self.reply(messages, 'math')['result']})
        self.assertNotIn('Console', {item['label'] for item in self.reply(messages, 'global')['result']})
        self.assertIn('union', {item['label'] for item in self.reply(messages, 'set')['result']})
        self.assertIn('(method) int[].sort(compareFn?: (int, int) => int): int[]', self.reply(messages, 'sort')['result']['contents']['value'])
        self.assertIn('(method) Math.sqrt(value: double): double', self.reply(messages, 'sqrt')['result']['contents']['value'])

    def test_class_hover_displays_constructor_parameters_defaults_and_documentation(self) -> None:
        source = ("/// A counter.\nclass Counter { value: int; step: int = -1; "
                  "label: string = 'Vela'; static other: int = 0; private hidden: bool = true; }\n"
                  "class Pair { value: int; constructor(value: int) { this.value = value; } }\n"
                  "func main(): void { Counter(value: 40); Pair(42); print(Error(message: 'bad')); }")
        messages = self.session([self.open(source),
            self.symbol_query('hover', source, source.index('Counter(value'), 'generated'),
            self.symbol_query('hover', source, source.index('Pair(42)'), 'explicit'),
            self.symbol_query('hover', source, source.index('Error(message'), 'error')])
        self.assertEqual(self.diagnostics(messages)[-1]['diagnostics'], [])
        generated = self.reply(messages, 'generated')['result']['contents']['value']
        self.assertIn("class Counter {\n  constructor(value: int, step: int = -1, label: string = 'Vela');\n}", generated)
        self.assertIn('A counter.', generated)
        self.assertNotIn('hidden:', generated)
        self.assertNotIn('other:', generated)
        self.assertIn('constructor(value: int);', self.reply(messages, 'explicit')['result']['contents']['value'])
        error = self.reply(messages, 'error')['result']['contents']['value']
        self.assertIn("constructor(message: string, code: string = 'Error');", error)
        self.assertIn('caught', error)

    def test_native_api_hover_and_completion_share_documentation(self) -> None:
        source = ('func main(): void { print(Math.sqrt(9.0)); print(performance.now()); '
                  'print(Date.now()); Random.seed(42); print(Math.PI); }')
        messages = self.session([self.open(source),
            self.symbol_query('hover', source, source.index('sqrt'), 'sqrt'),
            self.symbol_query('hover', source, source.index('performance.now') + len('performance.'), 'monotonic'),
            self.symbol_query('hover', source, source.index('Date.now') + len('Date.'), 'wall'),
            self.symbol_query('hover', source, source.index('seed'), 'seed'),
            self.symbol_query('hover', source, source.index('PI'), 'pi'),
            self.symbol_query('completion', source, source.index('Math.sqrt') + len('Math.'), 'completion'),
            self.symbol_query('completion', source, source.index('print('), 'global')])
        sqrt = self.reply(messages, 'sqrt')['result']['contents']['value']
        self.assertIn('(method) Math.sqrt(value: double): double', sqrt)
        self.assertIn('square root', sqrt)
        self.assertIn('Error', sqrt)
        self.assertIn('milliseconds', self.reply(messages, 'monotonic')['result']['contents']['value'])
        self.assertIn('1970', self.reply(messages, 'wall')['result']['contents']['value'])
        self.assertIn('64 bits', self.reply(messages, 'seed')['result']['contents']['value'])
        self.assertIn('Pi,', self.reply(messages, 'pi')['result']['contents']['value'])
        completion = next(item for item in self.reply(messages, 'completion')['result'] if item['label'] == 'sqrt')
        self.assertIn(completion['documentation']['value'], sqrt)
        math = next(item for item in self.reply(messages, 'global')['result'] if item['label'] == 'Math')
        self.assertIn('private constructor()', math['detail'])
        self.assertIn('Mathematical', math['documentation']['value'])

    def test_enum_interface_and_members_follow_typescript_hover_conventions(self) -> None:
        source = ("/// Available brightness levels.\nenum Brightness {\n  light,\n"
                  "  /// A dark appearance.\n  dark,\n}\n"
                  "/// A readable value.\ninterface Readable {\n"
                  "  /// Reads a scaled value.\n  read(scale: int): int;\n}\n"
                  "func use(reader: Readable): int { const mode = Brightness.dark; return reader.read(2); }\n"
                  "func main(): void {}")
        markers = [('Brightness {', 'enum'), ('Brightness.dark', 'enum_use'),
                   ('dark;', 'member'), ('Readable {', 'interface'), ('Readable):', 'interface_use'),
                   ('read(2)', 'method'), ('scale:', 'parameter')]
        messages = self.session([self.open(source), *[
            self.symbol_query('hover', source, source.index(marker), identity) for marker, identity in markers]])
        self.assertEqual(self.diagnostics(messages)[-1]['diagnostics'], [])
        for identity in ('enum', 'enum_use'):
            hover = self.reply(messages, identity)['result']['contents']['value']
            self.assertTrue(hover.startswith('```vela\nenum Brightness\n```'))
            self.assertIn('Available brightness levels.', hover)
        for identity in ('interface', 'interface_use'):
            hover = self.reply(messages, identity)['result']['contents']['value']
            self.assertTrue(hover.startswith('```vela\ninterface Readable\n```'))
            self.assertIn('A readable value.', hover)
        member = self.reply(messages, 'member')['result']['contents']['value']
        self.assertIn('(enum member) Brightness.dark = 1', member)
        self.assertIn('A dark appearance.', member)
        method = self.reply(messages, 'method')['result']['contents']['value']
        self.assertIn('(method) Readable.read(scale: int): int', method)
        self.assertIn('Reads a scaled value.', method)
        self.assertIn('(parameter) scale: int', self.reply(messages, 'parameter')['result']['contents']['value'])

    def test_type_alias_hover_retains_generic_parameters_and_function_grouping(self) -> None:
        source = ('/// Converts values to text.\ntype Handler<T> = (T) => string;\n'
                  'type Maybe<T> = T?;\ntype Handlers = ((int) => string)[];\n'
                  'type OptionalHandler = ((int) => string)?;\n'
                  'func main(): void { const convert: Handler<int> = value => value.toString(); '
                  'let maybe: Maybe<int> = null; const handlers: Handlers = []; '
                  'let optional: OptionalHandler = null; }')
        checks = [('Handler<T>', 'type Handler<T> = (T) => string'),
                  ('Handler<int>', 'type Handler<T> = (T) => string'),
                  ('Maybe<T>', 'type Maybe<T> = T?'), ('Maybe<int>', 'type Maybe<T> = T?'),
                  ('Handlers =', 'type Handlers = ((int) => string)[]'),
                  ('OptionalHandler =', 'type OptionalHandler = ((int) => string)?')]
        messages = self.check_hovers(source, checks)
        self.assertIn('Converts values to text.', self.reply(messages, '1')['result']['contents']['value'])

    def test_imported_member_hover_keeps_qualified_names_without_internal_prefixes(self) -> None:
        library = self.directory / 'library.vela'
        library.write_text('/// A readable value.\nexport interface Readable { read(scale: int): int; }\n'
                           'export enum Mode { light, dark }\nexport type Callback<T> = (T) => string;\n', encoding='utf-8')
        source = ("import 'library' as lib;\nfunc use(value: lib.Readable): int { "
                  "print(lib.Mode.dark); return value.read(2); }\n"
                  "func main(): void { const convert: lib.Callback<int> = value => value.toString(); }")
        checks = [('Readable)', 'interface Readable'), ('dark)', '(enum member) Mode.dark = 1'),
                  ('read(2)', '(method) Readable.read(scale: int): int'),
                  ('Callback<int>', 'type Callback<T> = (T) => string')]
        messages = self.check_hovers(source, checks)
        for index in range(len(checks)):
            hover = self.reply(messages, str(index))['result']['contents']['value']
            self.assertNotIn('$module', hover)

    def test_intrinsic_hover_explains_units_returns_and_mutation(self) -> None:
        source = ("func main(): void { const text = '你好'; const values = [1, 2]; "
                  "const keys = Set<int>(); const lookup = Map<int, string>(); "
                  "const builder = String(); print(text.slice(0, 1)); print(text.length); "
                  "print(values.slice(0, 1)); print(values.length); print(values.map(value => value + 1)); "
                  "print(lookup.get(1)); print(keys.add(1)); builder.append('x'); print(builder.toString()); }")
        checks = [('text.slice', 'slice', 'Unicode code point'), ('text.length', 'length', 'O(UTF-8'),
                  ('values.slice', 'slice', 'shallow copy'), ('values.length', 'length', 'O(1)'),
                  ('values.map', 'map', 'original array unchanged'), ('lookup.get', 'get', 'null'),
                  ('keys.add', 'add', 'false'), ('builder.append', 'append', 'amortized'),
                  ('builder.toString', 'toString', 'immutable')]
        messages = self.session([self.open(source), *[
            self.symbol_query('hover', source, source.index(fragment) + fragment.index(name), str(index))
            for index, (fragment, name, _) in enumerate(checks)]])
        self.assertEqual(self.diagnostics(messages)[-1]['diagnostics'], [])
        for index, (_, _, expected) in enumerate(checks):
            self.assertIn(expected, self.reply(messages, str(index))['result']['contents']['value'])

    def test_every_intrinsic_completion_has_documentation(self) -> None:
        source = ("func main(): void { const text = 'x'; const values = [1]; const keys = Set<int>(); "
                  "const lookup = Map<int, string>(); "
                  "const builder = String(); const integer = 1; const decimal = 1.5; const flag = true; "
                  "print(text.length); print(values.length); print(keys.size); print(lookup.size); "
                  "print(builder.toString()); print(integer.toString()); "
                  "print(decimal.toString()); print(flag.toString()); }")
        queries = [self.open(source)]
        for name, member in [('text', 'length'), ('values', 'length'), ('keys', 'size'), ('lookup', 'size'),
                             ('builder', 'toString'), ('integer', 'toString'),
                             ('decimal', 'toString'), ('flag', 'toString')]:
            queries.append(self.symbol_query('completion', source, source.index(name + '.' + member) + len(name) + 1, name))
        messages = self.session(queries)
        for name in ('text', 'values', 'keys', 'lookup', 'builder', 'integer', 'decimal', 'flag'):
            for item in self.reply(messages, name)['result']:
                with self.subTest(receiver=name, method=item['label']):
                    self.assertTrue(item.get('documentation', {}).get('value'), item)
                    self.assertTrue(item['documentation']['value'].isascii(), item)

    def test_mutable_string_name_is_shared_by_constructor_hover_completion_and_coloring(self) -> None:
        source = ("func main(): void { const text: String = String(); text.append('value'); "
                  "const snapshot: string = text.toString(); print(snapshot); }")
        messages = self.session([self.open(source),
            self.symbol_query('hover', source, source.index('String()'), 'constructor'),
            self.symbol_query('hover', source, source.index('text.append') + len('text.'), 'append'),
            self.symbol_query('hover', source, source.index('text.toString') + len('text.'), 'snapshot'),
            self.symbol_query('completion', source, source.index('String()'), 'types'),
            request('textDocument/semanticTokens/full', {'textDocument': {'uri': self.uri}}, 'tokens')])
        self.assertEqual(self.diagnostics(messages)[-1]['diagnostics'], [])
        self.assertIn('class String {\n  constructor();\n}', self.reply(messages, 'constructor')['result']['contents']['value'])
        self.assertIn('(method) String.append(text: string): void', self.reply(messages, 'append')['result']['contents']['value'])
        self.assertIn('(method) String.toString(): string', self.reply(messages, 'snapshot')['result']['contents']['value'])
        candidates = {item['label'] for item in self.reply(messages, 'types')['result']}
        self.assertIn('String', candidates)
        self.assertNotIn('StringBuilder', candidates)
        for spelling in ('String =', 'String()'):
            self.assertIn((0, source.index(spelling), len('String'), 'class', 2), self.semantic_tokens(messages))

    def test_removed_result_builtins_are_not_offered_in_completion(self) -> None:
        source = "func main(): void { print(42); }"
        messages = self.session([self.open(source),
            self.symbol_query('completion', source, source.index('print'), 'completion')])
        self.assertEqual(self.diagnostics(messages)[-1]['diagnostics'], [])
        candidates = {item['label'] for item in self.reply(messages, 'completion')['result']}
        self.assertIn('print', candidates)
        self.assertTrue(candidates.isdisjoint({'Expected', 'ok', 'err'}))

    def semantic_tokens(self, messages: list[dict], identity: str = 'tokens') -> list[tuple]:
        legend = self.reply(messages, 1)['result']['capabilities']['semanticTokensProvider']['legend']
        data = self.reply(messages, identity)['result']['data']
        self.assertEqual(len(data) % 5, 0)
        line = character = 0
        result = []
        for index in range(0, len(data), 5):
            delta_line, delta_character, length, kind, modifiers = data[index:index + 5]
            line += delta_line
            character = character + delta_character if delta_line == 0 else delta_character
            result.append((line, character, length, legend['tokenTypes'][kind], modifiers))
        return result

    def test_semantic_tokens_color_type_declarations_uses_and_generic_parameters(self) -> None:
        source = ("class Box<T> { value: T; }\ninterface Readable { read(): int; }\n"
                  "enum Mode { light, dark }\ntype IntBox = Box<int>;\n"
                  "func main(): void { const text = 'Box'; const value: IntBox = Box<int>(value: 42); "
                  "const mode: Mode = Mode.light; print(Math.PI); { const Box = 1; print(Box); } }")
        for encoding in ('utf-8', 'utf-16'):
            with self.subTest(encoding=encoding):
                messages = self.session([self.open(source), request('textDocument/semanticTokens/full',
                    {'textDocument': {'uri': self.uri}}, 'tokens')], encoding)
                self.assertEqual(self.diagnostics(messages)[-1]['diagnostics'], [])
                tokens = self.semantic_tokens(messages)
                self.assertIn((0, 6, 3, 'class', 1), tokens)
                self.assertIn((0, 10, 1, 'typeParameter', 1), tokens)
                self.assertIn((0, source.splitlines()[0].rindex('T;'), 1, 'typeParameter', 0), tokens)
                self.assertIn((1, 10, 8, 'interface', 1), tokens)
                self.assertIn((2, 5, 4, 'enum', 1), tokens)
                self.assertIn((3, 5, 6, 'type', 1), tokens)
                line = source.splitlines()[4]
                self.assertIn((4, line.index('Box<int>'), 3, 'class', 0), tokens)
                self.assertIn((4, line.index('Math.PI'), 4, 'class', 2), tokens)
                self.assertFalse(any(token[0] == 4 and token[1] >= line.index('const Box') for token in tokens))
                self.assertFalse(any(token[0] == 4 and token[1] == line.index("'Box'") + 1 for token in tokens))

    def test_semantic_tokens_use_import_overlays_and_invalidate_on_changes(self) -> None:
        library = self.directory / 'library.vela'
        library.write_text('export class Item { value: int; }', encoding='utf-8')
        source = "import 'library' as lib; func main(): void { const text = '中😀'; lib.Item(value: 1); }"
        for encoding in ('utf-8', 'utf-16'):
            with self.subTest(encoding=encoding):
                token_request = lambda identity: request('textDocument/semanticTokens/full', {'textDocument': {'uri': self.uri}}, identity)
                messages = self.session([self.open(source), token_request('disk'),
                    self.open('export func Item(value: int): int { return value; }', library.as_uri()),
                    token_request('overlay')], encoding)
                offset = source.index('Item(value:')
                column = len(source[:offset].encode('utf-8')) if encoding == 'utf-8' else len(source[:offset].encode('utf-16-le')) // 2
                self.assertIn((0, column, 4, 'class', 0), self.semantic_tokens(messages, 'disk'))
                self.assertFalse(any(token[1] == column for token in self.semantic_tokens(messages, 'overlay')))

    def argument_completions(self, marked_source: str, encoding: str = 'utf-16') -> list[dict]:
        offset = marked_source.index('¦')
        source = marked_source.replace('¦', '')
        messages = self.session([self.open(source),
            self.symbol_query('completion', source, offset, encoding=encoding)], encoding)
        return self.reply(messages, 'query')['result']

    def test_constructor_argument_completion_inserts_names_and_shows_types_and_defaults(self) -> None:
        items = self.argument_completions(
            'class Counter { value: int; step: int = 1; '
            'static other: int = 0; private unused: int = 0; constructor {} }\n'
            'func main(): void { const initial = 40; Counter(¦); }')
        parameters = {item['label']: item for item in items if item['label'].endswith(':')}
        self.assertEqual(parameters.keys(), {'value:', 'step:'})
        self.assertEqual(parameters['value:']['detail'], 'value: int')
        self.assertEqual(parameters['step:']['detail'], 'step: int = …')
        self.assertEqual(parameters['value:']['textEdit']['newText'], 'value: ')
        self.assertEqual(parameters['value:']['kind'], 5)
        initial = next(item for item in items if item['label'] == 'initial')
        self.assertLess(parameters['value:']['sortText'], initial['sortText'])

    def test_constructor_completes_unfinished_names_and_omits_supplied_arguments(self) -> None:
        header = 'class Counter { value: int; step: int = 1; }\nfunc main(): void { '
        for text, labels, insertion in (
                ('Counter(¦', {'value:', 'step:'}, 'value: '),
                ('Counter(¦); }', {'value:', 'step:'}, 'value: '),
                ('Counter(¦ }', {'value:', 'step:'}, 'value: '),
                ('Counter(va¦); }', {'value:'}, 'value: '),
                ('Counter(value: 40, ¦); }', {'step:'}, 'step: '),
                ('Counter(step: 1, va¦); }', {'value:'}, 'value: '),
                ('Counter(va¦lue: 40); }', {'value:'}, 'value'),
                ('Counter(¦value: 40, step: 1); }', {'value:'}, 'value')):
            with self.subTest(text=text):
                items = self.argument_completions(header + text)
                parameters = {item['label']: item for item in items if item['label'].endswith(':')}
                self.assertEqual(parameters.keys(), labels)
                item = parameters['value:' if 'value:' in labels else 'step:']
                self.assertEqual(item['textEdit']['newText'], insertion)
                edit = item['textEdit']
                line = text.replace('¦', '')
                begin = edit['range']['start']['character'] - len('func main(): void { ')
                end = edit['range']['end']['character'] - len('func main(): void { ')
                self.assertNotIn('valuevalue', line[:begin] + edit['newText'] + line[end:])

    def test_explicit_constructor_completion_uses_only_declared_parameters(self) -> None:
        items = self.argument_completions(
            'class Box { value: int; text: string = ""; private cache: int = 0; '
            'constructor(initial: int) { this.value = initial; } }\n'
            'func main(): void { Box(¦); }')
        parameters = {item['label'] for item in items if item['label'].endswith(':')}
        self.assertEqual(parameters, {'initial:'})

    def test_completion_excludes_removed_member_modifiers(self) -> None:
        source = 'func main(): void { print(42); }'
        messages = self.session([self.open(source),
            self.symbol_query('completion', source, source.index('print('), 'keywords')])
        labels = {item['label'] for item in self.reply(messages, 'keywords')['result']}
        self.assertIn('private', labels)
        self.assertNotIn('public', labels)
        self.assertNotIn('param', labels)

    def test_argument_completion_resolves_explicit_generic_imported_and_alias_constructors(self) -> None:
        library = self.directory / 'library.vela'
        library.write_text('export class Box<T> { value: T; }\n'
                           'export class Pair { first: int; second: string; '
                           'constructor(first: int, second: string) { '
                           'this.first = first; this.second = second; } }\n', encoding='utf-8')
        header = "import 'library' as lib;\ntype IntBox = lib.Box<int>;\nfunc main(): void { "
        for call, details in (('lib.Box<string>(¦)', {'value:': 'value: string'}),
                              ('IntBox(¦)', {'value:': 'value: int'}),
                              ('lib.Pair(40, ¦)', {'second:': 'second: string'})):
            with self.subTest(call=call):
                parameters = {item['label']: item['detail'] for item in self.argument_completions(header + call + '; }')
                              if item['label'].endswith(':')}
                self.assertEqual(parameters, details)

    def test_named_argument_completion_preserves_function_value_and_lexical_semantics(self) -> None:
        header = ('class Box { value: int; add(left: int, right: int): int { return left + right; } }\n'
                  'func sum(left: int, right: int): int { return left + right; }\n'
                  'func main(): void { const box = Box(value: 1); const operation = sum; ')
        for call, labels in (('sum(¦)', {'left:', 'right:'}), ('box.add(1, ¦)', {'right:'}),
                             ('operation(¦)', set()), ('Math.pow(¦)', {'left:', 'right:'})):
            with self.subTest(call=call):
                items = self.argument_completions(header + call + '; }')
                self.assertEqual({item['label'] for item in items if item['label'].endswith(':')}, labels)
                self.assertIn('box', {item['label'] for item in items})

    def test_argument_completion_ignores_nested_commas_and_respects_encoding(self) -> None:
        header = ('class Box<T, U> { value: T; }\n'
                  'func pair(left: int, right: int): int { return left + right; }\n'
                  'func run(values: int[], callback: (int, int) => int, value: Box<int, string>): void {}\n'
                  "func main(): void { const label = '中文😀'; ")
        for encoding in ('utf-8', 'utf-16'):
            for call, labels in (
                    ('run([pair(1, 2), 3], (a, b) => pair(a, b), ¦)', {'value:'}),
                    ('pair(1, ¦)', {'right:'}), ('Box<int, string>(¦)', {'value:'})):
                with self.subTest(encoding=encoding, call=call):
                    items = self.argument_completions(header + call + '; }', encoding)
                    self.assertEqual({item['label'] for item in items if item['label'].endswith(':')}, labels)

    def test_argument_names_are_not_suggested_in_values_comments_or_shadowed_calls(self) -> None:
        for source in (
                "class Box { value: string; } func main(): void { Box(value: 'hel¦lo'); }",
                'class Box { value: int; } func main(): void { const Box = 42; Box(¦); }',
                'class Box { value: int; } func main(): void { Box(value: ¦); }',
                'class Box { value: int[]; } func main(): void { Box(value: [¦]); }',
                'class Box { value: int; } func main(): void { // Box(¦\n}'):
            with self.subTest(source=source):
                items = self.argument_completions(source)
                self.assertFalse(any(item['label'].endswith(':') for item in items))

    def test_js_api_names_and_map_results_use_production_types(self) -> None:
        source = ("func main(): void { const numbers = [1, 2]; "
                  "const mapped = numbers.map(value => value.toString()); const keys = Set<int>(); "
                  "keys.add(1); print(keys.size); print(keys.delete(1)); print(mapped.join(',')); "
                  "print('x'.includes('x')); print(performance.now()); print(Date.now()); print(Math.random()); }")
        messages = self.session([
            self.open(source),
            self.symbol_query('completion', source, source.index('numbers.map') + len('numbers.'), 'array'),
            self.symbol_query('completion', source, source.index('keys.add') + len('keys.'), 'set'),
            self.symbol_query('completion', source, source.index('Math.random') + len('Math.'), 'math'),
            self.symbol_query('hover', source, source.index('mapped.join'), 'mapped'),
            self.symbol_query('hover', source, source.index('performance.now') + len('performance.'), 'monotonic'),
            self.symbol_query('hover', source, source.index('Date.now') + len('Date.'), 'wall')])
        self.assertEqual(self.diagnostics(messages)[-1]['diagnostics'], [])
        arrays = {item['label'] for item in self.reply(messages, 'array')['result']}
        self.assertTrue({'some', 'map'} <= arrays)
        self.assertFalse({'any', 'transform'} & arrays)
        sets = {item['label'] for item in self.reply(messages, 'set')['result']}
        self.assertTrue({'size', 'delete'} <= sets)
        self.assertFalse({'length', 'remove'} & sets)
        self.assertIn('random', {item['label'] for item in self.reply(messages, 'math')['result']})
        self.assertIn('const mapped: string[]', self.reply(messages, 'mapped')['result']['contents']['value'])
        self.assertIn('(method) performance.now(): double', self.reply(messages, 'monotonic')['result']['contents']['value'])
        self.assertIn('(method) Date.now(): int', self.reply(messages, 'wall')['result']['contents']['value'])

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
        self.assertIn('(method) string[].join(separator?: string): string',
                      self.reply(messages, 'hover')['result']['contents']['value'])

    def test_dynamic_array_methods_share_typed_completion_hover_and_argument_names(self) -> None:
        source = ("func main(): void { const values: int[] = []; values.reserve(100); "
                  "values.push(42, 40); values.splice(start: 0, deleteCount: 1); "
                  "const total = values.reduce((sum, value) => sum + value, 0); "
                  "const callbacks: ((int) => int)[] = []; callbacks.push(x => x); "
                  "const item = values.pop(); print(values.capacity); }")
        messages = self.session([
            self.open(source),
            self.symbol_query('completion', source, source.index('values.reserve') + len('values.'), 'methods'),
            self.symbol_query('hover', source, source.index('push('), 'push'),
            self.symbol_query('hover', source, source.index('pop('), 'pop'),
            self.symbol_query('hover', source, source.index('reduce('), 'reduce'),
            self.symbol_query('hover', source, source.index('callbacks.push') + len('callbacks.'), 'callback_push'),
            self.symbol_query('hover', source, source.index('values.capacity') + len('values.'), 'capacity'),
            self.symbol_query('completion', source, source.index('start: 0'), 'arguments')])
        self.assertEqual(self.diagnostics(messages)[-1]['diagnostics'], [])
        methods = {item['label']: item for item in self.reply(messages, 'methods')['result']}
        for name in ('push', 'pop', 'shift', 'unshift', 'splice', 'clear', 'reserve', 'capacity'):
            self.assertIn(name, methods)
            self.assertTrue(methods[name].get('documentation'), name)
        for name in ('insert', 'removeAt', 'copy'):
            self.assertNotIn(name, methods)
        self.assertIn('(...items: int[]): int', methods['push']['detail'])
        self.assertIn('(): int?', methods['pop']['detail'])
        self.assertIn('(method) int[].push(...items: int[]): int', self.reply(messages, 'push')['result']['contents']['value'])
        self.assertIn('Amortized O(1)', self.reply(messages, 'push')['result']['contents']['value'])
        self.assertIn('(method) int[].pop(): int?', self.reply(messages, 'pop')['result']['contents']['value'])
        self.assertIn('push(...items: ((int) => int)[]): int',
                      self.reply(messages, 'callback_push')['result']['contents']['value'])
        reduction = self.reply(messages, 'reduce')['result']['contents']['value']
        self.assertIn('initialValue: R', reduction)
        self.assertNotIn('initialValue?', reduction)
        self.assertLess(reduction.index('callbackFn:'), reduction.index('initialValue:'))
        self.assertIn('Read-only', self.reply(messages, 'capacity')['result']['contents']['value'])
        arguments = {item['label'] for item in self.reply(messages, 'arguments')['result']}
        self.assertIn('start:', arguments)
        self.assertNotIn('items:', arguments)

    def test_inferred_lambda_parameters_expose_hover_members_and_definitions(self) -> None:
        source = ("class Item { value: int; }\n"
                  "func main(): void { const values = [Item(value: 42)];\n"
                  "values.forEach(item => { print(item.value); });\n"
                  "const add: (int, int) => int = (left, right) => left + right; print(add(20, 22)); }")
        item = source.index('item.value')
        left = source.index('left + right')
        messages = self.session([
            self.open(source),
            self.symbol_query('hover', source, item, 'item'),
            self.symbol_query('hover', source, left, 'left'),
            self.symbol_query('completion', source, item + len('item.'), 'members'),
            self.symbol_query('definition', source, item, 'definition')])
        self.assertEqual(self.diagnostics(messages)[-1]['diagnostics'], [])
        self.assertIn('item: Item', self.reply(messages, 'item')['result']['contents']['value'])
        self.assertIn('left: int', self.reply(messages, 'left')['result']['contents']['value'])
        self.assertIn('value', {entry['label'] for entry in self.reply(messages, 'members')['result']})
        definition = self.reply(messages, 'definition')['result']
        self.assertEqual(definition['range']['start'], {'line': 2, 'character': len('values.forEach(')})

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
                   ("sqrt(9.0)", "(method) Math.sqrt(")]
        messages = self.check_hovers(source, queries)
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

    def test_reports_invalid_utf8_in_unopened_import_with_unicode_prefix(self) -> None:
        library = self.directory / "bad.vela"
        prefix = '// 中😀\r\nconst text = "中😀"; '
        library.write_bytes(prefix.encode('utf-8') + b'\xff')
        for encoding in ('utf-8', 'utf-16'):
            with self.subTest(encoding=encoding):
                messages = self.session([
                    self.open('import "bad.vela"; func main(): int { return 42; }')
                ], encoding)
                diagnostics = self.diagnostics(messages, library.as_uri())[-1]['diagnostics']
                self.assertTrue(diagnostics)
                column = prefix.split('\r\n')[-1]
                expected = (len(column.encode('utf-8')) if encoding == 'utf-8'
                            else len(column.encode('utf-16-le')) // 2)
                self.assertEqual(diagnostics[0]['range']['start'],
                                 {'line': 1, 'character': expected})

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

    def test_type_parameter_rename_tracks_bounds_and_shadowed_scopes(self) -> None:
        source = ("const banner = '😀'; interface Reader<E> { read(): E; }\n"
                  "class Box<T> { value: T;\n"
                  "  read<T extends Reader<int>, U extends T>(value: T, other: U): int {\n"
                  "    return value.read();\n  }\n}\n"
                  "func identity<T>(value: T): T { return value; }\n")
        offsets = [source.index('read<T') + 5, source.index('U extends T') + 10,
                   source.index('value: T, other') + 7]
        for encoding in ('utf-8', 'utf-16'):
            with self.subTest(encoding=encoding):
                rename = self.symbol_query('rename', source, offsets[-1], 'rename', encoding)
                rename['params']['newName'] = 'Element'
                messages = self.session([
                    self.open(source),
                    self.symbol_query('prepareRename', source, offsets[-1], 'prepare', encoding),
                    rename,
                    self.symbol_query('prepareRename', source, source.index('value.read'),
                                      'ordinary', encoding)], encoding)
                expected = []
                for offset in offsets:
                    start = self.symbol_query('', source, offset, encoding=encoding)['params']['position']
                    end = self.symbol_query('', source, offset + 1, encoding=encoding)['params']['position']
                    expected.append({'range': {'start': start, 'end': end}, 'newText': 'Element'})
                self.assertEqual(self.reply(messages, 'rename')['result'],
                                 {'changes': {self.uri: expected}})
                self.assertEqual(self.reply(messages, 'prepare')['result'], expected[-1]['range'])
                self.assertIsNone(self.reply(messages, 'ordinary')['result'])
                self.assertEqual(self.diagnostics(messages)[-1]['diagnostics'], [])

    def test_type_parameter_rename_rejects_capture_and_invalid_names(self) -> None:
        source = 'class Value {} func use<T, U>(value: T, other: Value): T { return value; }'
        messages = [self.open(source)]
        for name in ('U', 'Value', 'class', 'T,U', '', ' T'):
            rename = self.symbol_query('rename', source, source.index('<T') + 1, name)
            rename['params']['newName'] = name
            messages.append(rename)
        replies = self.session(messages)
        for name in ('U', 'Value', 'class', 'T,U', '', ' T'):
            self.assertEqual(self.reply(replies, name)['error']['code'], -32602)

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
        header = ("class Counter {\n  value: int;\n"
                  "  private secret: int = 1;\n  static total: int = 0;\n"
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
                ('const value = "text";', {"slice", "length", "byteLength", "includes", "graphemes"}),
                ("const value: int[] = [1];", {"slice", "splice", "fill", "reverse", "length"}),
                ("const value = Set<int>();", {"add", "has", "delete", "clear", "values", "size"}),
                ("const value = Map<string, int>();", {"get", "set", "keys", "values", "size"}),
                ("const value = 1;", {"toString", "toInt", "toDouble"})):
            with self.subTest(declaration=declaration):
                source = "func main(): void { " + declaration + " value.; }"
                offset = source.index("value.;") + len("value.")
                messages = self.session([self.open(source), self.symbol_query("completion", source, offset)])
                labels = {item["label"] for item in self.reply(messages, "query")["result"]}
                self.assertTrue(expected <= labels, labels)
        for guard, expected in (("", set()), ("if (value != null)", {"length", "includes"})):
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

    def test_sdk_imports_use_real_declarations_and_readonly_source_identity(self) -> None:
        source = "import 'sdk:test' as t; func main(): void { t.fail('failure'); }"
        messages = self.session([
            self.open(source),
            self.symbol_query('definition', source, source.index('t.fail') + 2, 'definition'),
            self.symbol_query('hover', source, source.index('t.fail') + 2, 'hover'),
            request('textDocument/documentLink', {'textDocument': {'uri': self.uri}}, 'links'),
            request('vela/sdkSource', {'uri': 'sdk:test'}, 'sdk'),
            request('vela/sdkSource', {'uri': self.uri}, 'disk'),
        ])
        self.assertEqual(self.diagnostics(messages)[-1]['diagnostics'], [])
        definition = self.reply(messages, 'definition')['result']
        self.assertEqual(definition['uri'], 'sdk:test')
        hover = self.reply(messages, 'hover')['result']['contents']['value']
        self.assertIn('fail(message: string)', hover)
        links = self.reply(messages, 'links')['result']
        self.assertEqual([link['target'] for link in links], ['sdk:test'])
        sdk = self.reply(messages, 'sdk')['result']
        self.assertEqual(sdk, (ROOT / 'compiler/sdk/test.vela').read_text(encoding='utf-8'))
        self.assertEqual(sdk.splitlines()[definition['range']['start']['line']],
                         'export func fail(message: string): void {')
        self.assertEqual(self.reply(messages, 'disk')['error']['code'], -32602)

    def test_sdk_matcher_members_and_optional_parameters_use_production_signatures(self) -> None:
        source = ("import 'sdk:test'; func main(): void { "
                  "const matcher = equals(42); matcher.match(42); test('case', () => {}); }")
        member = source.index('matcher.match') + len('matcher.match')
        messages = self.session([
            self.open(source),
            self.symbol_query('definition', source, member, 'member'),
            self.symbol_query('hover', source, member, 'hover'),
            self.symbol_query('hover', source, source.index("test('case'"), 'test'),
        ])
        self.assertEqual(self.diagnostics(messages)[-1]['diagnostics'], [])
        self.assertEqual(self.reply(messages, 'member')['result']['uri'], 'sdk:test')
        self.assertIn('match(actual: dynamic)', self.reply(messages, 'hover')['result']['contents']['value'])
        self.assertIn('skip: dynamic = null', self.reply(messages, 'test')['result']['contents']['value'])

    def test_generated_mocks_keep_real_signatures_navigation_and_readonly_sdk_sources(self) -> None:
        api = self.directory / 'api.vela'
        generated = self.directory / 'api.mocks.vela'
        api.write_text('export interface Repository { load(id: int): string; }\n', encoding='utf-8')
        result = subprocess.run([str(self.executable), 'mock', str(api), '--interface', 'Repository',
                                 '-o', str(generated)], capture_output=True, text=True,
                                encoding='utf-8', timeout=20, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        before = generated.read_bytes(), generated.stat().st_mtime_ns
        source = ("import 'sdk:mock' as mock;\nimport './api.mocks.vela' as generated;\n"
                  "func main(): void {\n"
                  "  const repository = generated.MockRepository(mock.MockSession());\n"
                  "  repository.object.load(id: 42);\n"
                  "  repository.calls.load(id: mock.any);\n}\n")
        real = source.index('object.load') + len('object.')
        pattern = source.index('calls.load') + len('calls.')
        arguments = source.index('calls.load(') + len('calls.load(')
        messages = self.session([
            self.open(source),
            self.symbol_query('hover', source, real, 'real'),
            self.symbol_query('hover', source, pattern, 'pattern'),
            self.symbol_query('definition', source, real, 'real-definition'),
            self.symbol_query('definition', source, pattern, 'pattern-definition'),
            self.symbol_query('completion', source, pattern, 'members'),
            self.symbol_query('completion', source, arguments, 'arguments'),
            self.symbol_query('definition', source, source.index('MockSession'), 'session'),
            request('vela/sdkSource', {'uri': 'sdk:mock'}, 'sdk'),
        ])
        self.assertEqual(self.diagnostics(messages)[-1]['diagnostics'], [])
        self.assertIn('load(id: int): string', self.reply(messages, 'real')['result']['contents']['value'])
        self.assertIn('load(id: dynamic)', self.reply(messages, 'pattern')['result']['contents']['value'])
        self.assertIn('Describes Repository.load', self.reply(messages, 'pattern')['result']['contents']['value'])
        self.assertEqual(self.reply(messages, 'real-definition')['result']['uri'], api.as_uri())
        self.assertEqual(self.reply(messages, 'pattern-definition')['result']['uri'], generated.as_uri())
        self.assertIn('load', [item['label'] for item in self.reply(messages, 'members')['result']])
        parameter = next(item for item in self.reply(messages, 'arguments')['result'] if item['label'] == 'id:')
        self.assertEqual(parameter['detail'], 'id: dynamic')
        self.assertEqual(self.reply(messages, 'session')['result']['uri'], 'sdk:mock')
        self.assertEqual(self.reply(messages, 'sdk')['result'],
                         (ROOT / 'compiler/sdk/mock.vela').read_text(encoding='utf-8'))
        self.assertEqual((generated.read_bytes(), generated.stat().st_mtime_ns), before)

        generated.unlink()
        messages = self.session([self.open(source)])
        self.assertTrue(self.diagnostics(messages)[-1]['diagnostics'])
        self.assertFalse(generated.exists())

    def test_definition_resolves_shadowed_locals_parameters_and_closure_captures(self) -> None:
        source = ("func main(seed: int): int {\n  const value = seed;\n"
                  "  { const value = 7; print(value); }\n"
                  "  const read = (): int => value;\n  return read();\n}")
        queries = [(source.index("= seed") + 2, 0, source.index("seed:")),
                   (source.index("print(value)") + 6, 2, len("  { const ")),
                   (source.index("=> value") + 3, 1, len("  const "))]
        self.check_definitions(source, queries)

    def test_definition_resolves_members_enums_and_type_annotations(self) -> None:
        source = ("enum Color { red, blue }\n"
                  "class Box { value: int; get(): int { return this.value; } }\n"
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

    def test_repeated_completion_snapshots_do_not_pollute_hover_or_later_edits(self) -> None:
        source = ("class Box { first: int; second: string; } "
                  "func main(): void { const box = Box(first: 42, second: 'x'); print(box.first); }")
        offset = source.index("box.first") + len("box.")
        changed = source.replace("first", "third")
        messages = self.session([
            self.open(source),
            self.symbol_query("completion", source, offset, "first"),
            self.symbol_query("completion", source, offset, "repeated"),
            self.symbol_query("hover", source, source.index("box.first"), "hover"),
            self.symbol_query("completion", source, offset, "after_hover"),
            self.change([{"text": changed}]),
            self.symbol_query("completion", changed, offset, "changed"),
        ])
        first = self.reply(messages, "first")["result"]
        self.assertEqual(first, self.reply(messages, "repeated")["result"])
        self.assertEqual(first, self.reply(messages, "after_hover")["result"])
        self.assertEqual({item["label"] for item in first}, {"first", "second"})
        self.assertIn("Box", self.reply(messages, "hover")["result"]["contents"]["value"])
        self.assertEqual({item["label"] for item in self.reply(messages, "changed")["result"]},
                         {"third", "second"})

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
        source = ("class Box { value: int; }\n"
                  "func add(left: int, right: int): int { return left + right; }\n"
                  "func main(): int { const box = Box(value: 1); return add(left: box.value, right: 2); }")
        queries = [(source.index("value: 1"), 0, source.index("value:")),
                   (source.index("left: box"), 1, len("func add(")),
                   (source.index("right: 2"), 1, source.splitlines()[1].index("right:"))]
        self.check_definitions(source, queries)

    def test_for_catch_and_match_bindings_stay_in_their_scopes(self) -> None:
        source = ("class Failure { message: string; }\n"
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

    def test_for_in_bindings_have_distinct_definitions_types_and_body_scopes(self) -> None:
        source = ("func main(): void {\n"
                  "  const items = [1];\n"
                  "  for (const items in items) { print(items); }\n"
                  "  const pairs = Map<string, int>(); pairs.set('key', 42);\n"
                  "  for (const [key, value] in pairs) {\n"
                  "    const capture = (): string => key; print(value);\n"
                  "  }\n"
                  "  print(items);\n}")
        markers = [('iterable', 'in items', 3), ('element', 'print(items)', 6),
                   ('key', '=> key', 3), ('value', 'print(value)', 6)]
        messages = self.session([self.open(source), *[
            self.symbol_query('definition', source, source.index(marker) + skip, identity)
            for identity, marker, skip in markers],
            self.symbol_query('hover', source, source.index('print(items)') + 6, 'element-type'),
            self.symbol_query('hover', source, source.index('=> key') + 3, 'key-type'),
            self.symbol_query('hover', source, source.index('print(value)') + 6, 'value-type'),
            self.symbol_query('completion', source, source.index('print(value)'), 'inside'),
            self.symbol_query('completion', source, source.rindex('print(items)'), 'outside')])
        self.assertEqual(self.diagnostics(messages)[-1]['diagnostics'], [])
        for identity, line, marker in [('iterable', 1, 'items ='), ('element', 2, 'items in'),
                                       ('key', 4, 'key,'), ('value', 4, 'value]')]:
            self.assertEqual(self.reply(messages, identity)['result']['range']['start'], {
                'line': line, 'character': source.splitlines()[line].index(marker)})
        for identity, signature in [('element-type', 'const items: int'),
                                     ('key-type', 'const key: string'),
                                     ('value-type', 'const value: int')]:
            self.assertIn(signature, self.reply(messages, identity)['result']['contents']['value'])
        inside = {item['label']: item for item in self.reply(messages, 'inside')['result']}
        self.assertEqual(inside['key']['kind'], 21)
        self.assertEqual(inside['value']['detail'], 'int')
        outside = {item['label'] for item in self.reply(messages, 'outside')['result']}
        self.assertNotIn('key', outside)
        self.assertNotIn('value', outside)

    def test_imported_base_type_resolves_to_its_declaration(self) -> None:
        library = self.directory / 'base.vela'
        library.write_text('export class Base<T> { value: T; }\n', encoding='utf-8')
        source = ("import 'base' as lib;\n"
                  "class Child<T> extends lib.Base<T> { constructor { super(value: this.extra); } extra: T; }\n"
                  "func main(): int { return Child<int>(extra: 42).value; }")
        messages = self.session([self.open(source),
            self.symbol_query('definition', source, source.index('Base<T>'))])
        self.assertEqual(self.diagnostics(messages)[-1]['diagnostics'], [])
        target = self.reply(messages, 'query')['result']
        self.assertEqual(target['uri'], library.as_uri())
        self.assertEqual(target['range']['start'], {'line': 0, 'character': len('export class ')})

    def test_generic_alias_interface_and_chained_member_queries(self) -> None:
        source = ("interface Readable { read(): int; }\n"
                  "class Box<T> { value: T; }\n"
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

    def test_async_members_have_typed_hover_completion_and_named_arguments(self) -> None:
        source = ("async func main(): Future<int> { const future = Future.value(40); "
                  "return await future.then((value) => value + 2); }")
        messages = self.session([self.open(source),
            self.symbol_query("hover", source, source.index(".then") + 2, "hover"),
            self.symbol_query("completion", source, source.index(".then") + 1, "members"),
            self.symbol_query("completion", source, source.index("Future.value") + len("Future."), "factories")])
        hover = self.reply(messages, "hover")["result"]["contents"]["value"]
        self.assertIn("Future<int>", hover)
        self.assertIn("continuation", hover)
        members = {item["label"] for item in self.reply(messages, "members")["result"]}
        self.assertTrue({"then", "catchError", "whenComplete", "timeout"} <= members)
        factories = {item["label"] for item in self.reply(messages, "factories")["result"]}
        self.assertTrue({"value", "error", "sync", "microtask", "delayed", "wait", "race"} <= factories)
        source = "async func main(): Future<int[]> { return await Future.wait<int>([], ea); }"
        messages = self.session([self.open(source),
            self.symbol_query("completion", source, source.index("ea)") + 2)])
        labels = {item["label"] for item in self.reply(messages, "query")["result"]}
        self.assertIn("eagerError:", labels)

    def test_builtin_method_values_keep_function_type_and_documentation(self) -> None:
        source = ("func main(): int { const values = [40]; "
                  "const copy: () => int[] = values.slice; return copy()[0] + 2; }")
        messages = self.session([self.open(source),
            self.symbol_query("hover", source, source.index("copy()"), "value"),
            self.symbol_query("hover", source, source.index(".slice") + 2, "method")])
        self.assertIn("() => int[]", self.reply(messages, "value")["result"]["contents"]["value"])
        self.assertIn("shallow copy", self.reply(messages, "method")["result"]["contents"]["value"])

    def test_numeric_case_and_date_members_share_checked_signatures(self) -> None:
        source = ("func main(): int { const parsed = int.parse('42'); "
                  "const text = 'Vela'.toLowerCase(); const parts = Date.toUTC(0); "
                  "return parsed + parts.year; }")
        messages = self.session([self.open(source),
            self.symbol_query("completion", source, source.index("int.parse") + len("int."), "numbers"),
            self.symbol_query("hover", source, source.index("int.parse") + len("int.p"), "parse"),
            self.symbol_query("hover", source, source.index(".toLowerCase") + 2, "case"),
            self.symbol_query("completion", source, source.index("Date.toUTC") + len("Date."), "date"),
            self.symbol_query("completion", source, source.index("parts.year") + len("parts."), "parts")])
        numbers = {item["label"] for item in self.reply(messages, "numbers")["result"]}
        self.assertTrue({"parse", "tryParse"} <= numbers)
        parse = self.reply(messages, "parse")["result"]["contents"]["value"]
        self.assertIn("radix", parse)
        self.assertIn("radix?: int?", parse)
        self.assertIn("int", parse)
        self.assertIn("Unicode", self.reply(messages, "case")["result"]["contents"]["value"])
        date = {item["label"] for item in self.reply(messages, "date")["result"]}
        self.assertTrue({"parse", "toISOString", "fromUTC", "toUTC", "now"} <= date)
        parts = {item["label"] for item in self.reply(messages, "parts")["result"]}
        self.assertTrue({"year", "month", "day", "hour", "minute", "second", "millisecond"} <= parts)
        self.assertNotIn("yearValue", parts)

    def test_byte_storage_views_and_endian_share_checked_api_metadata(self) -> None:
        source = ("func main(): void { const bytes = ByteArray(length: 8); "
                  "const view = ByteData.view(bytes); bytes.slice(); "
                  "view.setInt32(0, 42, endian: Endian.little); print(view.byteLength); }")
        messages = self.session([
            self.open(source),
            self.symbol_query("hover", source, source.index("ByteArray(") + 2, "owner"),
            self.symbol_query("completion", source, source.index("bytes.slice") + len("bytes."), "bytes"),
            self.symbol_query("completion", source, source.index("view.setInt32") + len("view."), "view"),
            self.symbol_query("hover", source, source.index(".setInt32") + 2, "write"),
            self.symbol_query("hover", source, source.index(".view(") + 2, "factory"),
            self.symbol_query("completion", source, source.index("Endian.little") + len("Endian."), "endian")])
        self.assertIn("constructor(length: int)", self.reply(messages, "owner")["result"]["contents"]["value"])
        owners = {item["label"] for item in self.reply(messages, "bytes")["result"]}
        self.assertTrue({"length", "slice"} <= owners)
        self.assertFalse({"push", "pop", "capacity", "from"} & owners)
        views = {item["label"] for item in self.reply(messages, "view")["result"]}
        self.assertTrue({"byteLength", "getInt8", "setInt8", "getUint16", "setUint32", "getInt64", "setFloat32", "getFloat64"} <= views)
        self.assertNotIn("getUint64", views)
        self.assertIn("endian?: Endian", self.reply(messages, "write")["result"]["contents"]["value"])
        self.assertIn("offset?: int", self.reply(messages, "factory")["result"]["contents"]["value"])
        self.assertIn("length?: int", self.reply(messages, "factory")["result"]["contents"]["value"])
        self.assertEqual({item["label"] for item in self.reply(messages, "endian")["result"]}, {"big", "little", "host"})

    def test_regexp_signatures_properties_and_named_arguments_share_metadata(self) -> None:
        source = ("func main(): void { const re = RegExp('(?<x>a)', dotAll: true); "
                  "const found = re.firstMatch('a'); re.allMatches('a'); "
                  "print(found?.groupNames); print(found?.group(0)); }")
        messages = self.session([
            self.open(source),
            self.symbol_query("hover", source, source.index("RegExp(") + 2, "ctor"),
            self.symbol_query("completion", source, source.index("re.firstMatch") + 3, "re"),
            self.symbol_query("completion", source, source.index("found?.groupNames") + 7, "found"),
            self.symbol_query("hover", source, source.index(".allMatches") + 2, "all"),
            self.symbol_query("hover", source, source.index("?.groupNames") + 3, "names"),
            self.symbol_query("hover", source, source.index("?.group(0)") + 3, "group"),
            self.symbol_query("completion", source, source.index("dotAll:") + 2, "args")])
        self.assertFalse(self.diagnostics(messages)[-1]["diagnostics"])
        self.assertIn("caseSensitive?: bool", self.reply(messages, "ctor")["result"]["contents"]["value"])
        self.assertTrue({"pattern", "isUnicode", "firstMatch", "allMatches", "hasMatch", "matchAsPrefix", "stringMatch"}
                        <= {item["label"] for item in self.reply(messages, "re")["result"]})
        self.assertTrue({"start", "end", "input", "pattern", "groupCount", "groupNames", "group", "groups", "namedGroup"}
                        <= {item["label"] for item in self.reply(messages, "found")["result"]})
        self.assertIn("start?: int", self.reply(messages, "all")["result"]["contents"]["value"])
        self.assertIn("RegExpMatch[]", self.reply(messages, "all")["result"]["contents"]["value"])
        self.assertIn("string[]", self.reply(messages, "names")["result"]["contents"]["value"])
        self.assertIn("string?", self.reply(messages, "group")["result"]["contents"]["value"])
        self.assertIn("dotAll:", {item["label"] for item in self.reply(messages, "args")["result"]})

    def test_definition_accepts_identifier_end_before_punctuation_and_whitespace(self) -> None:
        source = ("class Box { value: int; }\n"
                  "func answer(): int { return 42; }\n"
                  "func main(): int { const box = Box(value: 1); print(box.value); return answer(); }")
        queries = [(source.index("answer();") + len("answer"), 1, len("func ")),
                   (source.index("box.value") + len("box"), 2, source.splitlines()[2].index("box =")),
                   (source.index("box.value") + len("box.value"), 0, source.index("value:"))]
        self.check_definitions(source, queries)

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
