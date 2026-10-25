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
                self.assertIsNone(self.reply(replies, 999)["result"])

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
        self.assertEqual(edits[0]["newText"], 'func main(): int {\n  print("must not run");\n  return 42;\n}\n')
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


if __name__ == "__main__":
    unittest.main()
