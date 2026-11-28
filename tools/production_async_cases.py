"""Eager async workloads for the production scheduler comparison."""

DECLARATIONS = """
async func productionTask(input: Future<int>): Future<int> {
  const live = [40, 2];
  return await input + live[0] + live[1];
}
"""


def async_cases() -> list[dict]:
    records = []
    def add(name, kind, size, expected, body):
        records.append(dict(name='async/' + name, kind=kind, size=size, expected=expected,
                            setup='', body=body, asynchronous=True))
    add('no_await', 'AsyncNoAwait', 1, 42, 'return 42;')
    add('completed_await', 'AsyncCompleted', 1, 42, 'return await Future.value(42);')
    add('suspend_resume', 'AsyncSuspend', 1, 42, 'return await Future.microtask(() => 42);')
    add('then_chain/64', 'AsyncChain', 64, 64,
        'let task = Future.value(0); for (let i = 0; i < 64; i++) { '
        'task = task.then(value => value + 1); } return await task;')
    for count in (1, 100, 1000):
        add(f'concurrent/{count}', 'AsyncConcurrent', count, count * 42,
            'const producer = Completer<int>(); const tasks: Future<int>[] = []; '
            f'for (let i = 0; i < {count}; i++) {{ tasks.push(productionTask(producer.future)); }} '
            'Timer.after(0, (): void => { producer.complete(0); }); '
            'const values = await Future.wait(tasks); let total = 0; '
            'for (const value in values) { total += value; } return total;')
        add(f'timers/{count}', 'AsyncTimers', count, count,
            f'let total = 0; for (let i = 0; i < {count}; i++) {{ '
            'Timer.after(0, (): void => { total++; }); } await sleep(0); return total;')
    return records
