"""Exercise mock generation and execution through the shipped command line."""

from __future__ import annotations

import json
import os
import unittest

from tools.test_support import CommandTestCase


class MockCommandTest(CommandTestCase):
    directory_prefix = "vela-mock-"

    def generate(self, *options: str, code: int = 0):
        return self.invoke("mock", "api.vela", "--interface", "Repository",
                           "-o", "repository.mocks.vela", *options, code=code)

    def report_events(self, name: str, code: int = 0) -> list[dict]:
        self.write("vela.json", json.dumps({"test": {"paths": [name]}}))
        result = self.invoke("test", "--reporter=json", code=code)
        return [json.loads(line) for line in result.stdout.splitlines()]

    def test_generated_interface_runs_through_all_vm_entry_paths(self):
        self.write("api.vela", """
export interface Repository {
  load(id: int): Future<string>;
  save(id: int, name: string): void;
  current(): int?;
}
""")
        self.generate()
        self.generate("--check")
        self.invoke("check", "repository.mocks.vela")
        self.write("repository_test.vela", """
import 'sdk:test';
import 'sdk:mock' as mock;
import './repository.mocks.vela' as generated;
func main(): void {
  test('typed interface', async (): Future<void> => {
    const repository = generated.MockRepository(session: mock.MockSession());
    mock.when(repository.calls.load(id: 42)).thenAnswer(
      async (invocation: mock.MockInvocation): Future<string> => {
        expect(invocation.argument('id'), 42);
        return 'Alice';
      });
    mock.when(repository.calls.save(42, 'Alice')).thenDo(
      (invocation: mock.MockInvocation): void => {});
    mock.when(repository.calls.current()).thenReturn(null);
    const observation = mock.untilCalled(repository.calls.save(mock.any, mock.any));
    const name = await repository.object.load(42);
    repository.object.save(name: name, id: 42);
    expect((await observation).arguments, [42, 'Alice']);
    expect(repository.object.current(), null);
    mock.verifyInOrder([repository.calls.load(42), repository.calls.save(42, 'Alice'),
                        repository.calls.current()]);
    mock.verifyNoMoreInteractions(repository.control);
  });
}
""")
        self.invoke("check", "repository_test.vela")
        self.invoke("run", "repository_test.vela")
        caches = list((self.directory / '.cache' / 'vela').glob('*.vlcache'))
        self.assertEqual(len(caches), 1)
        cached = [(path.read_bytes(), path.stat().st_mtime_ns) for path in caches]
        self.invoke("run", "repository_test.vela")
        self.assertEqual([(path.read_bytes(), path.stat().st_mtime_ns) for path in caches], cached)
        self.invoke("compile", "repository_test.vela", "suite.vbc")
        self.invoke("exec", "suite.vbc")
        self.write("vela.json", json.dumps({"test": {"paths": ["repository_test.vela"]}}))
        result = self.invoke("test", "--reporter=json")
        events = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertTrue(events[-1]["success"])
        self.assertEqual(events[-1]["passed"], 1)
        api = self.directory / 'api.vela'
        api.write_text(api.read_text(encoding='utf-8').replace('  current(): int?;',
                       '  current(): int?;\n  added(value: int): void;'), encoding='utf-8', newline='\n')
        self.generate('--check', code=1)
        self.generate()
        self.invoke('run', 'repository_test.vela')
        self.assertNotEqual([(path.read_bytes(), path.stat().st_mtime_ns) for path in caches], cached)
        api.write_text(api.read_text(encoding='utf-8').replace('  added(value: int): void;\n', ''),
                       encoding='utf-8', newline='\n')
        self.generate()
        self.assertNotIn('added(', (self.directory / 'repository.mocks.vela').read_text(encoding='utf-8'))

    def test_check_atomicity_stability_and_dependency_protection(self):
        api = self.write("api.vela", "export interface Repository { load(id: int): int; }")
        self.generate("--check", code=1)
        self.assertFalse((self.directory / "repository.mocks.vela").exists())
        self.generate()
        artifact = self.directory / "repository.mocks.vela"
        before = artifact.read_bytes(), artifact.stat().st_mtime_ns
        self.generate("--interface", "Repository")
        self.assertEqual((artifact.read_bytes(), artifact.stat().st_mtime_ns), before)
        self.assertNotIn(str(self.directory).encode(), artifact.read_bytes())
        self.write("api.vela", "export interface Repository { load(key: int): int; }")
        self.generate("--check", code=1)
        self.assertEqual((artifact.read_bytes(), artifact.stat().st_mtime_ns), before)
        self.generate()
        self.assertIn("key: int", artifact.read_text(encoding="utf-8"))
        current = artifact.read_bytes()
        self.write("api.vela", "export interface Repository { broken(): Missing; }")
        self.generate(code=65)
        self.assertEqual(artifact.read_bytes(), current)
        self.write("api.vela", "export interface Repository { load(): int; }")
        original = api.read_bytes()
        self.invoke("mock", "api.vela", "--interface", "Repository", "-o", "api.vela", code=64)
        self.assertEqual(api.read_bytes(), original)

    def test_aliases_containers_functions_and_auxiliary_name_collisions(self):
        self.write("other.vela", "export class Box<T> { value: T; }")
        self.write("api.vela", """
import './other.vela' as other;
class Private { value: int; }
export type Public = Private;
export func makePublic(): Public { return Private(value: 42); }
export enum Choice { first, second }
export interface Repository {
  calls(_control: int, _method0: string, _object: bool): other.Box<int>;
  object(): (int, string?) => int[];
  control(value: Public): Public;
  six(a: int, b: string, c: bool, d: int?, e: double, f: Map<string, int>): void;
  done(choice: Choice): Future<void>;
}
func main(): void { throw Error(message: 'generation must not execute'); }
""")
        self.generate()
        self.invoke("check", "repository.mocks.vela")
        source = (self.directory / "repository.mocks.vela").read_text(encoding="utf-8")
        self.assertIn(".Public", source)
        self.assertIn(".Box<int>", source)
        self.assertNotIn(".Private", source)
        self.write('types_test.vela', """import 'sdk:test';
import 'sdk:mock' as mock;
import './api.vela' as api;
import './other.vela' as other;
import './repository.mocks.vela' as generated;
func main(): void {
  test('all structural signature forms', async (): Future<void> => {
    const repository = generated.MockRepository(mock.MockSession());
    const box = other.Box<int>(value: 42);
    const value = api.makePublic();
    mock.when(repository.calls.calls(1, 'text', true)).thenReturn(box);
    mock.when(repository.calls.object()).thenReturn((number: int, text: string?): int[] => [number]);
    mock.when(repository.calls.control(mock.any)).thenReturn(value);
    mock.when(repository.calls.six(mock.any, mock.any, mock.any, mock.any, mock.any, mock.any))
      .thenDo((call: mock.MockInvocation): void => { expect(call.argument('d'), null); });
    mock.when(repository.calls.done(api.Choice.second)).thenAnswer(
      async (call: mock.MockInvocation): Future<void> => { await sleep(1); });
    expect(repository.object.calls(_object: true, _method0: 'text', _control: 1), same(box));
    const callback = repository.object.object();
    expect(callback(7, null), [7]);
    expect(repository.object.control(value), same(value));
    repository.object.six(f: Map<string, int>(), e: 2.0, d: null, c: false, b: 'x', a: 1);
    await repository.object.done(api.Choice.second);
    mock.verify(repository.calls.done(api.Choice.second)).called(1);
  });
}
""")
        self.invoke('run', 'types_test.vela')
        self.write("api.vela", "class Hidden {} export interface Repository { load(): Hidden; }")
        result = self.generate(code=65)
        self.assertIn("has no public name", result.stderr)

    def test_timeout_and_unfinished_configuration_are_real_test_failures(self):
        self.write("wait_test.vela", """
import 'sdk:test';
import 'sdk:mock' as mock;
func main(): void {
  test('wait', () => {
    const callback = mock.MockVoidFunction0(mock.MockSession());
    mock.untilCalled(callback.query());
  }, timeout: Timeout(5));
  test('unfinished', () => {
    const callback = mock.MockFunction0<int>(mock.MockSession());
    mock.when(callback.query());
  });
}
""")
        events = self.report_events("wait_test.vela", code=1)
        errors = [event for event in events if event["type"] == "error"]
        self.assertIn("TestTimeout", [error["category"] for error in errors])
        self.assertNotIn("AsyncIdle", [error["category"] for error in errors])
        self.assertTrue(any(error["phase"] == "tearDown" and "not completed" in error["message"]
                            for error in errors))
        self.assertEqual(events[-1]["failed"], 2)

    def test_reports_use_sdk_identity_and_preserve_actual_configuration_and_verification_locations(self):
        source = """import 'sdk:test';
import 'sdk:mock' as mock;
class Fake { message: string = 'MockMissingStub'; }
func main(): void {
  test('missing', () => {
    const callback = mock.MockFunction1<int, int>(mock.MockSession());
    mock.when(callback.query(1)).thenReturn(42);
    callback.function(2);
  });
  test('verification', () => {
    const callback = mock.MockFunction1<int, int>(mock.MockSession());
    mock.when(callback.query(mock.any)).thenReturn(42);
    callback.function(3);
    mock.verify(callback.query(3)).called(2);
  });
  test('ordinary business failure', () => { throw Fake(); });
}
"""
        self.write("report_test.vela", source)
        events = self.report_events("report_test.vela", code=1)
        errors = [event for event in events if event["type"] == "error"]
        self.assertEqual(len(errors), 3)
        missing, verification, ordinary = errors
        self.assertEqual(missing["category"], "MockMissingStub")
        self.assertEqual(verification["category"], "MockVerificationFailure")
        self.assertNotEqual(ordinary["category"], "MockMissingStub")
        self.assertNotIn("mock", ordinary)
        details = missing["mock"]
        self.assertGreater(details["mockSessionID"], 0)
        self.assertEqual(details["mockID"], 1)
        self.assertEqual(details["calls"][0]["sequence"], 1)
        self.assertEqual(details["calls"][0]["arguments"], ["2"])
        self.assertTrue(any(frame["line"] == 8 and frame["file"].endswith("report_test.vela")
                            for frame in details["calls"][0]["trace"]))
        self.assertTrue(any(frame["line"] == 7 and frame["file"].endswith("report_test.vela")
                            for frame in details["configurationTraces"][0]))
        details = verification["mock"]
        self.assertEqual(details["actualCount"], 1)
        self.assertEqual(details["calls"][0]["arguments"], ["3"])
        self.assertTrue(any(frame["line"] == 14 and frame["file"].endswith("report_test.vela")
                            for frame in details["verificationTrace"]))
        human = self.invoke("test", "--reporter=expanded", code=1)
        self.assertIn("#1 MockFunction1.function(2)", human.stdout)
        self.assertIn("configured MockFunction1.function(1)", human.stdout)

    def test_wait_matcher_failure_keeps_original_stack_and_does_not_interrupt_response(self):
        self.write("wait_test.vela", """import 'sdk:test';
import 'sdk:mock' as mock;
class BusinessError { message: string = 'original matcher failure'; }
class Broken implements Matcher {
  match(value: dynamic): MatchResult { throw BusinessError(); }
}
let callback: mock.MockFunction1<int, int>? = null;
func main(): void {
  setUp(() => {
    const function = mock.MockFunction1<int, int>(mock.MockSession());
    callback = function;
    mock.when(function.query(mock.any)).thenReturn(42);
    mock.untilCalled(function.query(Broken()));
  });
  test('matcher failure', () => {
    const function = callback as mock.MockFunction1<int, int>;
    expect(function.function(1), 42);
    mock.verify(function.query(1)).called(1);
    print('response completed');
  });
}
""")
        events = self.report_events("wait_test.vela", code=1)
        errors = [event for event in events if event["type"] == "error"]
        self.assertEqual(len(errors), 1)
        error = errors[0]
        self.assertEqual(error["message"], "original matcher failure")
        self.assertEqual(error["phase"], "setUp")
        self.assertIn("hookID", error)
        self.assertEqual(error["stack"][0]["line"], 5)
        self.assertTrue(error["stack"][0]["file"].endswith("wait_test.vela"))
        self.assertTrue(any(event["type"] == "print" and event["message"] == "response completed"
                            for event in events))

    def test_reset_waits_keep_separate_registration_locations(self):
        self.write("reset_test.vela", """import 'sdk:test';
import 'sdk:mock' as mock;
func main(): void {
  test('reset waits', () => {
    const callback = mock.MockFunction0<int>(mock.MockSession());
    mock.untilCalled(callback.query());
    mock.untilCalled(callback.query());
    mock.reset(callback.control);
  });
}
""")
        errors = [event for event in self.report_events("reset_test.vela", code=1)
                  if event["type"] == "error"]
        self.assertEqual([error["category"] for error in errors], ["MockReset", "MockReset"])
        locations = [{frame["line"] for frame in error["mock"]["verificationTrace"]
                      if frame["file"].endswith("reset_test.vela")} for error in errors]
        self.assertIn(6, locations[0])
        self.assertIn(7, locations[1])

    def test_generation_distinguishes_io_errors_and_rejects_input_aliases(self):
        self.generate(code=74)
        self.write('api.vela', "import './missing.vela'; export interface Repository {}")
        self.generate(code=74)
        source = self.write('api.vela', 'export interface Repository {}')
        alias = self.directory / 'alias.vela'
        os.link(source, alias)
        self.invoke('mock', 'api.vela', '--interface', 'Repository', '-o', 'alias.vela', code=64)
        self.assertEqual(source.read_text(encoding='utf-8'), 'export interface Repository {}')
        self.assertEqual(alias.read_bytes(), source.read_bytes())
        if os.name == 'nt':
            self.invoke('mock', 'api.vela', '--interface', 'Repository', '-o', 'API.VELA', code=64)

    def test_public_generic_alias_names_an_underlying_private_signature_type(self):
        self.write('api.vela', '''class Hidden<T> { value: T; }
export type Public<T> = Hidden<T>;
export interface Repository { exchange(value: Hidden<int>): Hidden<int>; }
''')
        self.generate()
        artifact = (self.directory / 'repository.mocks.vela').read_text(encoding='utf-8')
        self.assertIn('.Public<int>', artifact)
        self.assertNotIn('.Hidden<int>', artifact)
        self.invoke('check', 'repository.mocks.vela')
        self.write('api.vela', '''class Hidden<A, B> { first: A; second: B; }
class Marker {}
export type General<A, B> = Hidden<A, B>;
export type Specific<T> = Hidden<Marker, T>;
export interface Repository { exchange(value: General<Marker, int>): Hidden<Marker, int>; }
''')
        self.generate()
        artifact = (self.directory / 'repository.mocks.vela').read_text(encoding='utf-8')
        self.assertIn('.Specific<int>', artifact)
        self.assertNotIn('.Marker', artifact)
        self.invoke('check', 'repository.mocks.vela')

    def test_selection_order_and_duplicate_names_from_distinct_imports_are_stable(self):
        self.write('left.vela', 'export class Value { left: int; }')
        self.write('right.vela', 'export class Value { right: string; }')
        self.write('api.vela', """import './left.vela' as left;
import './right.vela' as right;
export interface Repository { use(a: left.Value, b: right.Value): left.Value; } export interface Empty {}
""")
        self.generate('--interface', 'Empty')
        artifact = self.directory / 'repository.mocks.vela'
        before = artifact.read_bytes(), artifact.stat().st_mtime_ns
        self.invoke('mock', 'api.vela', '--interface', 'Empty', '--interface', 'Repository',
                    '-o', 'repository.mocks.vela')
        self.assertEqual((artifact.read_bytes(), artifact.stat().st_mtime_ns), before)
        self.invoke('check', 'repository.mocks.vela')

    def test_unrelated_calls_do_not_extend_wait_timeout_and_cleanup_still_runs(self):
        self.write('timeout_test.vela', """import 'sdk:test';
import 'sdk:mock' as mock;
func main(): void {
  test('unrelated calls', () => {
    const callback = mock.MockFunction1<int, int>(mock.MockSession());
    mock.when(callback.query(mock.any)).thenReturn(42);
    mock.untilCalled(callback.query(999));
    const timer = Timer.periodic(1, () => { callback.function(0); });
    addTearDown(() => { timer.cancel(); print('timer cancelled'); });
  }, timeout: Timeout(5));
}
""")
        self.write('vela.json', '{"test":{"paths":["timeout_test.vela"]}}')
        result = self.invoke('test', '--reporter=json', code=1)
        events = [json.loads(line) for line in result.stdout.splitlines()]
        errors = [event for event in events if event['type'] == 'error']
        self.assertIn('TestTimeout', [error['category'] for error in errors])
        self.assertNotIn('AsyncIdle', [error['category'] for error in errors])
        self.assertTrue(any(event['type'] == 'print' and event['message'] == 'timer cancelled'
                            for event in events))

    def test_closed_sessions_and_cross_test_reuse_fail_without_changing_callers_scope(self):
        self.write('scope_test.vela', """import 'sdk:test';
import 'sdk:mock' as mock;
let retained: mock.MockFunction0<int>? = null;
func main(): void {
  test('first', () => {
    const session = mock.MockSession();
    const callback = mock.MockFunction0<int>(session);
    retained = callback;
    mock.when(callback.query()).thenReturn(42);
    expect(callback.function(), 42);
    session.close(); session.close();
    let closed = false;
    try { callback.function(); }
    catch (error: mock.MockSessionClosed) { closed = true; }
    expect(closed, true);
  });
  test('second', () => {
    const callback = retained as mock.MockFunction0<int>;
    let wrongScope = false;
    try { mock.verify(callback.query()).called(1); }
    catch (error: mock.MockUsageError) { wrongScope = true; }
    expect(wrongScope, true);
    const current = mock.MockFunction0<int>(mock.MockSession());
    mock.when(current.query()).thenReturn(7);
    expect(current.function(), 7);
  });
}
""")
        self.write('vela.json', '{"test":{"paths":["scope_test.vela"]}}')
        result = self.invoke('test', '--reporter=json')
        events = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(events[-1]['passed'], 2)

    def test_sessions_reject_registration_and_shared_group_setup(self):
        for main in ('const session = mock.MockSession();',
                     "setUpAll(() => { mock.MockSession(); }); test('blocked', () => {});"):
            with self.subTest(main=main):
                self.write('scope_test.vela', "import 'sdk:test'; import 'sdk:mock' as mock; "
                           'func main(): void { ' + main + ' }')
                self.write('vela.json', '{"test":{"paths":["scope_test.vela"]}}')
                result = self.invoke('test', '--reporter=json', code=1)
                events = [json.loads(line) for line in result.stdout.splitlines()]
                self.assertTrue(any(event.get('category') == 'MockUsageError' for event in events))

    def test_late_call_after_timeout_stays_with_original_test(self):
        self.write('late_test.vela', """import 'sdk:test';
import 'sdk:mock' as mock;
func main(): void {
  test('original', () => {
    const callback = mock.MockFunction0<int>(mock.MockSession());
    mock.when(callback.query()).thenReturn(42);
    mock.untilCalled(callback.query());
    Timer.after(10, () => { callback.function(); });
  }, timeout: Timeout(1));
  test('next', async (): Future<void> => { await sleep(20); });
}
""")
        self.write('vela.json', '{"test":{"paths":["late_test.vela"]}}')
        result = self.invoke('test', '--reporter=json', code=1)
        events = [json.loads(line) for line in result.stdout.splitlines()]
        identities = {event['name']: event['testID'] for event in events if event['type'] == 'test'}
        errors = [event for event in events if event['type'] == 'error']
        self.assertTrue(any(event['category'] == 'MockUsageError' for event in errors))
        self.assertTrue(all(event['testID'] == identities['original'] for event in errors))
        self.assertEqual(events[-1]['passed'], 1)
        self.assertEqual(events[-1]['failed'], 1)


if __name__ == "__main__":
    unittest.main()
