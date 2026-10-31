"""Validate source testing through the shipped CLI and its versioned event stream."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import unittest

from tools.test_support import CommandTestCase


class SourceTestingTest(CommandTestCase):
    directory_prefix = "vela-testing-"

    def configure(self, **settings) -> None:
        self.write("vela.json", json.dumps({"test": settings}))

    def suite(self, name: str = "unit/math_test.vela", body: str = "expect(42, 42);") -> Path:
        return self.write(name, "import 'sdk:test';\nfunc main(): void {\n"
                          f"test('case', () => {{ {body} }});\n}}\n")

    def events(self, *arguments: str, code: int = 0, cwd: Path | None = None) -> list[dict]:
        result = self.invoke("test", "--reporter=json", *arguments, code=code, cwd=cwd)
        self.assertEqual(result.stderr, "")
        events = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual([event["sequence"] for event in events], list(range(1, len(events) + 1)))
        self.assertEqual([event["time"] for event in events], sorted(event["time"] for event in events))
        self.assertTrue(all(event["protocol"] == "vela.test" and event["version"] == 1 for event in events))
        self.assertEqual(events[0]["type"], "start")
        self.assertEqual(events[-1]["type"], "done")
        self.assertEqual(events[-1]["success"], code == 0)
        return events

    def test_discovery_is_sorted_deduplicated_and_rooted_at_the_nearest_configuration(self):
        self.configure(paths=["unit", "unit/math_test.vela"], concurrency=1)
        self.suite()
        self.suite("unit/a_test.vela")
        self.suite("unit/ignored.vela", "fail('not selected');")
        nested = self.directory / "unit" / "nested"
        nested.mkdir()
        for cwd in (self.directory, nested):
            events = self.events(cwd=cwd)
            suites = [event for event in events if event["type"] == "suite"]
            self.assertEqual([event["file"] for event in suites], ["unit/a_test.vela", "unit/math_test.vela"])
            self.assertEqual(events[-1]["passed"], 2)
        events = self.events("unit/ignored.vela", code=1)
        self.assertEqual(events[-1]["failed"], 1)

    def test_explicit_config_and_paths_use_the_calling_directory(self):
        self.write("project/vela.json", '{"test":{}}')
        self.suite("explicit.vela")
        events = self.events("--config=project/vela.json", "explicit.vela")
        self.assertEqual(events[-1]["passed"], 1)
        self.assertEqual(next(event["file"] for event in events if event["type"] == "suite"), "../explicit.vela")

    def test_filename_glob_and_symlink_identity_do_not_follow_directory_cycles(self):
        self.configure(paths=["unit"], filename="{a,b}[0-9]_test.vela", concurrency=1)
        first = self.suite("unit/a1_test.vela")
        self.suite("unit/b2_test.vela")
        self.suite("unit/aX_test.vela", "fail('excluded');")
        events = self.events()
        self.assertEqual([event["file"] for event in events if event["type"] == "suite"],
                         ["unit/a1_test.vela", "unit/b2_test.vela"])
        try:
            (self.directory / "unit" / "loop").symlink_to(self.directory / "unit", target_is_directory=True)
            (self.directory / "alias.vela").symlink_to(first)
        except OSError as error:
            self.skipTest(f"symbolic links unavailable: {error}")
        events = self.events("unit", "alias.vela", str(first))
        self.assertEqual(events[-1]["passed"], 2)

    def test_configuration_and_discovery_are_fixed_for_the_running_session(self):
        self.configure(paths=["unit"], concurrency=1)
        self.write("unit/a_test.vela", "import 'sdk:test'; func main(): void {"
                   "test('wait', async (): Future<void> => { await sleep(20); }); }")
        self.suite("unit/b_test.vela")
        process = subprocess.Popen([str(self.executable), "test", "--reporter=json"],
                                   cwd=self.directory, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, encoding="utf-8")
        self.addCleanup(lambda: process.kill() if process.poll() is None else None)
        start = json.loads(process.stdout.readline())
        self.assertEqual(start["type"], "start")
        self.configure(paths=["absent"], concurrency=1, filename="none.vela")
        self.suite("unit/created_later_test.vela", "fail('outside discovery snapshot');")
        output, errors = process.communicate(timeout=30)
        self.assertEqual(process.returncode, 0, output + errors)
        events = [json.loads(line) for line in output.splitlines()]
        self.assertEqual(events[-1]["passed"], 2)
        self.assertEqual(len([event for event in events if event["type"] == "suite"]), 2)

    def test_runner_configuration_changes_preserve_valid_compilation_cache(self):
        self.configure(paths=["unit"])
        self.suite()
        self.events()
        cache = list((self.directory / ".cache" / "vela" / "test").glob("*.vlcache"))
        self.assertEqual(len(cache), 1)
        before = [(path.stat().st_mtime_ns, path.read_bytes()) for path in cache]
        self.configure(paths=["unit"], concurrency=2, timeout="1m", reporter="expanded")
        self.events("--list", "--plain-name=case")
        self.assertEqual([(path.stat().st_mtime_ns, path.read_bytes()) for path in cache], before)

    def test_no_implicit_paths_and_strict_input_errors(self):
        self.suite()
        self.invoke("test", code=64)
        self.configure()
        self.invoke("test", code=64)
        self.write("vela.json", "{}")
        self.invoke("test", "unit", code=64)
        malformed = ['{"test":{"paths":["unit"],"concurrency":0}}',
                     '{"test":{"paths":["unit"],"timeout":"1000000000000000s"}}',
                     '{"test":{"paths":["unit"],"tags":{"a":{},"a":{}}}}',
                     '{"test":{"paths":["unit"],"unknown":true}}',
                     '{"test":{"paths":["unit"],}}']
        for source in malformed:
            with self.subTest(source=source):
                self.write("vela.json", source)
                result = self.invoke("test", code=64)
                self.assertIn("vela.json:", result.stderr)
        self.configure(paths=["unit"])
        self.invoke("test", "absent.vela", code=74)
        for option in ("--unknown", "--name=[", "--tags=a &&", "--concurrency=0", "--reporter=other"):
            with self.subTest(option=option):
                self.invoke("test", option, code=64)

    def test_list_filters_duplicates_tags_and_registration_side_effects(self):
        self.configure(paths=["unit"], tags={"unit": {}, "slow": {}})
        self.write("unit/list_test.vela", """import 'sdk:test';
func main(): void {
  print('register');
  setUpAll(() => { fail('hook must not run'); });
  group('group', () => {
    for (let index = 0; index < 2; index++) {
      test('same', () => { fail('body must not run'); }, tags: 'unit');
    }
    test('slow', () => {}, tags: 'slow');
  });
}
""")
        events = self.events("--list", "--name=group.*", "--plain-name=same", "--tags=unit && !slow")
        definitions = [event for event in events if event["type"] == "test"]
        self.assertEqual(len(definitions), 2)
        self.assertNotEqual(definitions[0]["testID"], definitions[1]["testID"])
        self.assertEqual([event["name"] for event in definitions], ["group same", "group same"])
        self.assertTrue(all(event["tags"] == ["unit"] and event["location"]["line"] > 0 for event in definitions))
        self.assertEqual([event["message"] for event in events if event["type"] == "print"], ["register"])
        self.assertFalse(any(event["type"] == "testStart" for event in events))
        self.assertEqual(events[-1]["selected"], 2)
        self.assertEqual(events[-1]["passed"], 0)

    def test_empty_results_and_all_skipped_are_distinct(self):
        self.configure(paths=["unit"])
        (self.directory / "unit").mkdir()
        self.events(code=1)
        self.suite(body="fail('skip body');")
        self.events("--plain-name=absent", code=1)
        self.write("unit/math_test.vela", "import 'sdk:test'; func main(): void {"
                   "setUpAll(() => { fail('skip hook'); });"
                   "test('skip', () => { fail('skip body'); }, skip: 'documented'); }")
        events = self.events()
        self.assertEqual(events[-1]["passed"], 0)
        self.assertEqual(events[-1]["skipped"], 1)
        self.assertEqual(next(event for event in events if event["type"] == "test")["skipReason"], "documented")

    def test_late_errors_update_final_counts_and_keep_the_original_identity(self):
        self.configure(paths=["unit"])
        self.write("unit/late_test.vela", """import 'sdk:test';
func main(): void {
  test('first', () => { Timer.after(5, () => { fail('late'); }); print('first output'); });
  test('second', async (): Future<void> => { await sleep(20); print('second output'); });
}
""")
        events = self.events(code=1)
        first = next(event for event in events if event["type"] == "test" and event["name"] == "first")
        done = next(event for event in events if event["type"] == "testDone" and event["testID"] == first["testID"])
        error = next(event for event in events if event["type"] == "error")
        self.assertEqual(done["status"], "passed")
        self.assertEqual(error["testID"], first["testID"])
        self.assertLess(done["sequence"], error["sequence"])
        self.assertEqual(events[-1]["passed"], 1)
        self.assertEqual(events[-1]["failed"], 1)
        self.assertTrue(all("testID" in event for event in events if event["type"] == "print"))

    def test_source_failure_does_not_stop_independent_suites_and_has_exit_priority(self):
        self.configure(paths=["unit"], concurrency=2)
        self.suite("unit/ok_test.vela", "print('still ran');")
        self.suite("unit/fail_test.vela", "expect([1, 2], equals([1, 3]));")
        self.write("unit/bad_test.vela", "func main(: void {}")
        events = self.events(code=65)
        self.assertEqual(events[-1]["passed"], 1)
        self.assertEqual(events[-1]["failed"], 1)
        failure = next(event for event in events if event.get("category") == "TestFailure")
        self.assertEqual(failure["difference"]["path"], "[1]")
        self.assertIn("stack", failure)
        self.assertIn("location", failure)

    def test_parallel_suites_have_independent_module_globals_and_print_streams(self):
        self.configure(paths=["unit"], concurrency=4)
        self.write("unit/shared.vela", "let count = 0; export func next(): int { count++; return count; }")
        source = "import 'sdk:test'; import './shared.vela'; func main(): void {" \
                 "test('isolated', async (): Future<void> => { expect(next(), 1);" \
                 "await sleep(5); print('isolated output'); expect(next(), 2); }); }"
        for index in range(8):
            self.write(f"unit/{index}_test.vela", source)
        events = self.events()
        self.assertEqual(events[-1]["passed"], 8)
        outputs = [event for event in events if event["type"] == "print"]
        self.assertEqual(len(outputs), 8)
        self.assertEqual(len({event["suiteID"] for event in outputs}), 8)

    def test_direct_run_and_bytecode_use_the_same_sdk_session_without_config(self):
        path = self.suite("direct.vela", "expectLater(Future.value(42), completion(42)); print('captured');")
        result = self.invoke("run", str(path))
        self.assertIn("1 passed, 0 failed", result.stdout)
        image = self.directory / "direct.vbc"
        self.invoke("compile", str(path), str(image))
        self.assertIn("1 passed, 0 failed", self.invoke("exec", str(image)).stdout)
        self.invoke("check", str(path))
        documentation = self.directory / "sdk.md"
        self.invoke("doc", "sdk:test", str(documentation))
        rendered = documentation.read_text(encoding="utf-8")
        self.assertIn("interface Matcher", rendered)
        self.assertIn("skip: dynamic = null", rendered)
        formatted = self.invoke("fmt", str(path)).stdout
        self.write("formatted.vela", formatted)
        self.invoke("fmt", "--check", "formatted.vela")
        self.suite("direct.vela", "fail('visible failure');")
        self.assertIn("visible failure", self.invoke("run", str(path), code=1).stdout)

    def test_reporters_are_plain_when_redirected_and_cli_overrides_config(self):
        self.configure(paths=["unit"], reporter="json", timeout="1ms", concurrency=1)
        self.suite(body="print('hello');")
        for reporter in ("compact", "expanded"):
            result = self.invoke("test", "--reporter=" + reporter, "--timeout=1s", "--concurrency=2")
            self.assertNotIn("\x1b", result.stdout)
            self.assertIn("1 passed, 0 failed", result.stdout)
            self.assertIn("hello", result.stdout)

    def test_empty_prints_and_timeout_registration_locations_remain_structured(self):
        self.configure(paths=["unit"], timeout="2ms")
        self.suite(body="print(''); expectAsync0(() => {});")
        events = self.events(code=1)
        self.assertEqual([event["message"] for event in events if event["type"] == "print"], [""])
        error = next(event for event in events if event["type"] == "error")
        self.assertEqual(error["category"], "TestTimeout")
        self.assertEqual(error["location"]["line"], 3)

    def test_load_timeout_and_async_registration_errors_are_reported(self):
        self.configure(paths=["unit"], suite_load_timeout="0ms")
        self.suite()
        events = self.events(code=1)
        self.assertTrue(any(event.get("category") == "SuiteLoadTimeout" for event in events))
        self.configure(paths=["unit"])
        self.write("unit/math_test.vela", "import 'sdk:test'; async func main(): Future<void> {"
                   "await sleep(1); group('invalid', async (): Future<void> => {}); }")
        self.events(code=1)


if __name__ == "__main__":
    unittest.main()
