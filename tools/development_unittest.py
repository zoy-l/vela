"""Exercise editing and stopping the shipped JIT development command over real pipes."""

from __future__ import annotations

import queue
import re
import subprocess
import threading
import time
import unittest

from tools.test_support import CommandTestCase


class DevelopmentCommandTest(CommandTestCase):
    directory_prefix = "vela-development-command-"

    def setUp(self) -> None:
        super().setUp()
        self.entry = self.directory / "main.vela"
        self.lines: queue.Queue[tuple[str, str]] = queue.Queue()
        self.history: list[tuple[str, str]] = []
        self.pending: list[tuple[str, str]] = []

    def write(self, source: str) -> None:
        self.entry.write_text(source, encoding="utf-8", newline="\n")

    def start(self, *options: str) -> None:
        self.process = subprocess.Popen(
            [str(self.executable), "dev", str(self.entry), *options], cwd=self.directory,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", bufsize=1)
        self.readers = []
        for name, stream in [("out", self.process.stdout), ("err", self.process.stderr)]:
            reader = threading.Thread(target=self.read, args=(name, stream), daemon=True)
            reader.start()
            self.readers.append(reader)
        self.addCleanup(self.close)
        self.wait("err", "Vela JIT development session")

    def read(self, name: str, stream) -> None:
        for line in stream:
            self.lines.put((name, line.rstrip("\r\n")))

    def wait(self, stream: str, pattern: str) -> str:
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            for index, (name, line) in enumerate(self.pending):
                if name == stream and re.search(pattern, line):
                    self.pending.pop(index)
                    return line
            try:
                name, line = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            self.history.append((name, line))
            if name == stream and re.search(pattern, line):
                return line
            self.pending.append((name, line))
        self.fail(f"missing {stream} output {pattern!r}: {self.history!r}")

    def send(self, command: str) -> None:
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()

    def close(self) -> None:
        if self.process.poll() is None:
            try:
                self.send(".quit")
                self.process.wait(timeout=3)
            except (BrokenPipeError, OSError, subprocess.TimeoutExpired):
                self.process.kill()
                self.process.wait(timeout=3)
        self.process.stdin.close()
        for reader in self.readers:
            reader.join(timeout=3)
        self.process.stdout.close()
        self.process.stderr.close()

    def test_file_hot_reload_keeps_state_and_explicit_reload_reexecutes_entry(self) -> None:
        self.write("let score = 10; func main(): void { score = 42; } "
                   "export func read(): int { return score; }")
        self.start()
        self.send(".call read")
        self.wait("out", "^42$")
        self.write("let score = 100; func main(): void { score = 999; } "
                   "export func read(): int { return score + 1; }")
        self.wait("err", r"accepted \(run 1, revision 2\)")
        self.send(".call read")
        self.wait("out", "^43$")
        self.send(".reload")
        self.wait("err", r"accepted \(run 2, revision 1\)")
        self.send(".call read")
        self.wait("out", "^1000$")
        self.send(".quit")
        self.assertEqual(self.process.wait(timeout=5), 0)

    def test_quit_stops_a_nonallocating_infinite_script(self) -> None:
        self.write("func main(): void { while (true) {} }")
        self.start()
        self.send(".quit")
        self.assertEqual(self.process.wait(timeout=5), 0)

    def test_reload_exits_an_old_infinite_frame_and_starts_the_new_entry(self) -> None:
        self.write("func main(): void { while (true) {} }")
        self.start()
        self.write("func main(): void { print('new run'); } "
                   "export func read(): int { return 42; }")
        self.send(".reload")
        self.wait("out", "^new run$")
        self.wait("err", r"accepted \(run 2, revision 1\)")
        self.send(".call read")
        self.wait("out", "^42$")

    def test_failed_save_keeps_the_running_version_and_recovers_on_the_next_save(self) -> None:
        self.write("func main(): void {} export func read(): int { return 42; }")
        self.start()
        self.write("func main(): void {} export func read(): int { return true; }")
        self.wait("err", "source error")
        self.send(".call read")
        self.wait("out", "^42$")
        self.write("func main(): void {} export func read(): int { return 43; }")
        self.wait("err", r"accepted \(run 1, revision 2\)")
        self.send(".call read")
        self.wait("out", "^43$")

    def test_paused_old_frame_keeps_its_source_after_publication(self) -> None:
        self.write("func main(): void {}\n"
                   "export func caller(): int {\n"
                   "  print('old caller');\n"
                   "  return 42;\n"
                   "}\n")
        self.start()
        self.send(".break 3")
        self.wait("err", "breakpoint .*:3")
        self.send(".call caller")
        self.wait("err", "paused #0 caller revision 1.*:3:")
        self.write("func main(): void {}\n"
                   "export func caller(): int {\n"
                   "  print('new caller');\n"
                   "  return 100;\n"
                   "}\n")
        self.wait("err", r"accepted \(run 1, revision 2\)")
        self.send(".source")
        self.wait("err", r"3 \|.*old caller")
        self.send(".step")
        self.wait("out", "^old caller$")
        self.wait("err", "paused #0 caller revision 1")
        self.send(".continue")
        self.wait("out", "^42$")
        self.send(".call caller")
        self.wait("err", "paused #0 caller revision 2.*:3:")
        self.send(".source")
        self.wait("err", r"3 \|.*new caller")
        self.send(".continue")
        self.wait("out", "^new caller$")
        self.wait("out", "^100$")
        self.send(".gc")
        self.wait("err", "collection completed")
        self.send(".status")
        self.wait("err", "1 code batch")

    def test_optimized_paused_frame_deoptimizes_and_cached_code_remains_reloadable(self) -> None:
        self.write("func main(): void {}\n"
                   "export func caller(): int {\n"
                   "  print('old optimized frame');\n"
                   "  return 42;\n}\n")
        cache = self.directory / "objects"
        self.start("--optimized", f"--code-cache={cache}")
        self.send(".watch off")
        self.send(".break 3")
        self.wait("err", "breakpoint .*:3")
        self.send(".call caller")
        self.wait("err", r"paused #0 caller revision 1.*\[optimized\]")
        self.write("func main(): void {}\n"
                   "export func caller(): int {\n"
                   "  print('new optimized frame');\n"
                   "  return 100;\n}\n")
        self.send(".hot")
        self.wait("err", r"accepted \(run 1, revision 2\)")
        self.send(".stack")
        self.wait("err", r"#0 caller revision 1.*\[baseline\]")
        self.send(".continue")
        self.wait("out", "^old optimized frame$")
        self.wait("out", "^42$")
        self.send(".call caller")
        self.wait("err", r"paused #0 caller revision 2.*\[optimized\]")
        self.send(".continue")
        self.wait("out", "^new optimized frame$")
        self.wait("out", "^100$")
        self.send(".gc")
        self.wait("err", "collection completed")
        self.send(".status")
        self.wait("err", "1 code batch")
        self.wait("err", r"code [1-9][0-9]* bytes")
        self.wait("err", "optimized, 1 deoptimized frame")
        self.assertGreaterEqual(len(list(cache.glob("*.vlobj"))), 2)

    def test_pause_and_reload_can_stop_a_paused_infinite_frame(self) -> None:
        self.write("func main(): void { while (true) {} }")
        self.start()
        self.send(".pause")
        self.wait("err", "paused #0 main revision 1")
        self.send(".stack")
        self.wait("err", "^#0 main revision 1")
        self.write("func main(): void { print('resumed in new run'); }")
        self.send(".reload")
        self.wait("err", r"accepted \(run 2, revision 1\)")
        self.wait("out", "^resumed in new run$")

    def test_breakpoint_can_pause_and_continue_a_lazy_initializer(self) -> None:
        self.write("let value = initial();\n"
                   "func initial(): int { return 42; }\n"
                   "func main(): void {}\n"
                   "export func read(): int { return value; }\n")
        self.start()
        self.send(".break 2")
        self.wait("err", "breakpoint .*:2")
        self.send(".call read")
        self.wait("err", "paused #0 initial revision 1")
        self.send(".continue")
        self.wait("out", "^42$")

    def test_jit_repl_appends_without_replaying_and_uses_explicit_file_updates(self) -> None:
        self.write("func main(): void {}\nexport func answer(): int { return 40; }\n")
        self.start()
        self.send(".watch off")
        self.send("import 'main.vela' as app;")
        self.send("let value = app.answer();")
        self.send("value = value + 2;")
        self.send("value")
        self.wait("out", "^42$")
        self.send("let value = 999;")
        self.wait("err", "source error|duplicate")
        self.write("func main(): void {}\nexport func answer(): int { return 100; }\n")
        self.send("app.answer()")
        self.wait("out", "^40$")
        self.send(".hot")
        self.wait("err", r"accepted \(run 1, revision 7\)")
        self.send("app.answer()")
        self.wait("out", "^100$")
        self.send("value")
        self.wait("out", "^42$")
        self.write("func main(): void {}\nexport func answer(): int { return 200; }\n")
        self.send(".reset")
        self.wait("err", r"accepted \(run 2, revision 1\)")
        self.send("import 'main.vela' as app;")
        self.send("app.answer()")
        self.wait("out", "^200$")
        self.send("let value = 7;")
        self.send("value")
        self.wait("out", "^7$")

    def test_consecutive_update_requests_are_serialized_in_an_active_run(self) -> None:
        self.write("func main(): void { while (true) {} }\n")
        self.start()
        self.send(".watch off")
        self.write("func main(): void { while (true) {} }\n"
                   "export func answer(): int { return 42; }\n")
        self.send(".hot")
        self.send(".hot")
        self.wait("err", r"accepted \(run 1, revision 3\)")
        self.send(".status")
        self.wait("err", r"accepted \(run 1, revision 3\)")
        self.assertFalse(any("another request is pending" in line for _, line in self.history))


if __name__ == "__main__":
    unittest.main()
