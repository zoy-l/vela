"""Shared CLI and standalone AOT program lifecycle acceptance cases."""

OUTPUT_FAILURE_SOURCE = ("func main(): string { let text = 'x'; "
                         "for (let i = 0; i < 20; i++) { text += text; } return text; }")

# 输出超过 C++ 流缓冲区，使测试能在入口挂起期间观察到执行标记。
PENDING_SOURCE = ("func main(): Future<int> { let marker = 'ready'; "
                  "for (let i = 0; i < 14; i++) { marker += marker; } print(marker); "
                  "return Future.race<int>([]); }")
PENDING_OUTPUT_PREFIX = b"readyready"

PROGRAM_CASES = [
    ("scalar", "func main(): int { return 42; }", 0, "42\n"),
    ("negative", "func main(): int { return -1; }", 0, "-1\n"),
    ("large", "func main(): int { return 256; }", 0, "256\n"),
    ("maximum", "func main(): int { return 9223372036854775807; }", 0, "9223372036854775807\n"),
    ("minimum", "func main(): int { return -9223372036854775807 - 1; }", 0, "-9223372036854775808\n"),
    ("double", "func main(): double { return 1.5; }", 0, "1.5\n"),
    ("string", "func main(): string { print('first'); return '你好'; }", 0, "first\n你好\n"),
    ("boolean", "func main(): bool { return true; }", 0, "true\n"),
    ("null", "func main(): int? { return null; }", 0, "null\n"),
    ("void", "func main(): void { print('ready'); }", 0, "ready\n"),
    ("object", "class Box { value: int; } func main(): Box { return Box(value: 42); }", 0, "object\n"),
    ("array", "func main(): int[] { return [40, 2]; }", 0, "object\n"),
    ("await", "async func main(): Future<int> { print('first'); await 0; print('last'); return 42; }", 0, "first\nlast\n42\n"),
    ("future_void", "async func main(): Future<void> { await 0; print('ready'); }", 0, "ready\n"),
    ("future_null", "async func main(): Future<int?> { await 0; return null; }", 0, "null\n"),
    ("future_string", "async func main(): Future<string> { await sleep(0); return 'ready'; }", 0, "ready\n"),
    ("future_boolean", "async func main(): Future<bool> { await 0; return true; }", 0, "true\n"),
    ("future_array", "async func main(): Future<int[]> { await 0; return [42]; }", 0, "object\n"),
    ("future_object", "class Box { value: int; } async func main(): Future<Box> { await 0; return Box(value: 42); }", 0, "object\n"),
    ("returned_future", "func main(): Future<int> { return Future.value(42); }", 0, "42\n"),
    ("dynamic_future", "func main(): dynamic { return Future.value('ready'); }", 0, "ready\n"),
    ("dynamic_void_future", "async func ready(): Future<void> { await 0; } func main(): dynamic { return ready(); }", 0, ""),
    ("sync_background", "func main(): int { Future.microtask((): void => { print('background'); }); Timer.after(0, (): void => { print('timer'); }); return 42; }", 0, "42\n"),
    ("timer", "async func main(): Future<int> { Timer.after(0, (): void => { print('timer'); }); return 42; }", 0, "42\n"),
    ("periodic", "async func main(): Future<int> { Timer.periodic(100000, (): void => { print('tick'); }); await 0; return 42; }", 0, "42\n"),
    ("periodic_boundary", "func main(): Future<int> { const c = Completer<int>(); Timer.periodic(1, (): void => { c.complete(42); Future.microtask((): void => { print('after'); }); print('tick'); }); return c.future; }", 0, "tick\n42\n"),
    ("microtask_boundary", "func main(): Future<int> { const c = Completer<int>(); Future.microtask((): void => { c.complete(42); print('unit'); }); Future.microtask((): void => { print('after'); }); return c.future; }", 0, "unit\n42\n"),
    ("composition", "async func main(): Future<int> { return await Future.value(20).then((v: int): int => v + 22); }", 0, "42\n"),
    ("handled", "async func main(): Future<int> { try { await Future.error<int>(Error(message: 'handled')); } catch (error: Error) { return 42; } return 0; }", 0, "42\n"),
    ("discarded_error", "func main(): int { Future.microtask((): void => { throw Error(message: 'background'); }); return 42; }", 0, "42\n"),
    ("reported_background_error", "async func main(): Future<int> { Future.microtask((): void => { throw Error(message: 'background'); }); await sleep(0); return 42; }", 1, ""),
    ("failure", "func main(): int { print('before'); return 1 / 0; }", 1, "before\n"),
    ("async_failure", "async func fail(): Future<int> { await 0; print('before'); return 1 / 0; } async func main(): Future<int> { return await fail(); }", 1, "before\n"),
    ("standard", "func main(): int { return Math.sqrt(1764.0).toInt(); }", 0, "42\n"),
    ("typed_json", "class Item { value: int = 42; } func main(): int { const failed = JSON.safeParse<Item>('{\"value\":false}'); return !failed.success && failed.error.path == '$.value' ? JSON.parse<Item>('{}').value : 0; }", 0, "42\n"),
]
