"""Matched AOT/C++ production workload contracts."""

SEED = 42
DECLARATIONS = """
class Item { value: int; }
struct Pair { first: int; second: int; }
let integers: int[] = [];
let objects: Item[] = [];
let pairs: Pair[] = [];
let tagged: dynamic[] = [];
let seedValue: int = 0;
"""


def container_cases() -> list[dict]:
    cases = []
    def add(name: str, kind: str, size: int, expected: int, setup: str, body: str) -> None:
        cases.append(dict(name='containers/' + name, kind=kind, size=size,
                          expected=expected, setup=setup, body=body))

    setup = ('integers = []; seedValue = seed; integers.reserve(size); '
             'for (let i = 0; i < size; i++) { integers.push(seed + i); }')
    for count in (64, 4096):
        kept = [SEED + index for index in range(count) if (SEED + index) % 2 == 0]
        expected = len(kept) + sum((index + 1) * value for index, value in enumerate(kept))
        add(f'filter/{count}', 'Filter', count, expected, setup,
            'const result = integers.filter(value => value % 2 == 0); '
            'let total = result.length; '
            'for (let i = 0; i < result.length; i++) { total += (i + 1) * result[i]; } return total;')
        for operation in ('find', 'includes'):
            found = ('const value = integers.find(value => value == target); '
                     'if (value == null || value != target) { return 0; }' if operation == 'find'
                     else 'if (!integers.includes(target)) { return 0; }')
            add(f'{operation}/late/{count}', operation.title(), count, SEED, setup,
                'for (let offset = 1; offset <= 16; offset++) { '
                'const target = seedValue + integers.length - offset; ' + found + '} return 42;')
            missing = ('integers.find(value => value == -1) == null' if operation == 'find'
                       else '!integers.includes(-1)')
            add(f'{operation}/missing/{count}', operation.title() + 'Missing', count, SEED, setup,
                f'return {missing} ? 42 : 0;')
    add('filter/narrow', 'NarrowFilter', 4, 87, '',
        "const mixed: dynamic[] = [1, 'text', 42, null]; "
        'const result: int[] = mixed.filter(value => { '
        'if (value is int) { return value > 0; } return false; }); '
        'let total = result.length; '
        'for (let i = 0; i < result.length; i++) { total += (i + 1) * result[i]; } return total;')
    storage = [
        ('objects', 'IncludesObjects', 'objects', 'Item(value: 1)',
         'objects.includes(Item(value: 42))'),
        ('structs', 'IncludesStructs', 'pairs', 'Pair(first: 1, second: 2)',
         'pairs.includes(Pair(first: 1, second: 42))'),
        ('tagged_integers', 'IncludesIntegers', 'tagged', '1', 'tagged.includes(42)'),
        ('tagged_doubles', 'IncludesDoubles', 'tagged', '1.0', 'tagged.includes(42.0)'),
    ]
    for name, kind, array, element, test in storage:
        add('includes/' + name, kind, 4096, SEED,
            f'{array} = []; {array}.reserve(size); '
            f'for (let i = 0; i < size; i++) {{ {array}.push({element}); }}',
            f'return !{test} ? 42 : 0;')
    add('includes/NaN_zero', 'IncludesNaN', 4096, SEED,
        'tagged = []; tagged.reserve(size + 2); '
        'for (let i = 0; i < size; i++) { tagged.push(1.0); } '
        'tagged.push(Math.pow(-1.0, 0.5)); tagged.push(-0.0);',
        'return tagged.includes(Math.pow(-1.0, 0.5)) && tagged.includes(0.0) && '
        '!tagged.includes(42.0) ? 42 : 0;')
    for name, count, kind in [('empty', 0, 'Map'), ('small', 4, 'Map'), ('large', 128, 'Map'),
                              ('duplicates', 128, 'MapDuplicates'), ('dynamic', 32, 'MapDynamic'),
                              ('nested', 32, 'MapNested')]:
        entries = []
        final = {}
        for index in range(count):
            key = index % 8 if kind == 'MapDuplicates' else index
            final[key] = index + 1
            text = str(key)
            if kind == 'MapDynamic' and index % 2:
                text += '.0'
            if kind == 'MapNested':
                text = f"'key{key}'"
            value = f'[{index + 1}]' if kind == 'MapNested' else str(index + 1)
            entries.append(text + ': ' + value)
        arguments = {'MapDynamic': 'dynamic, dynamic', 'MapNested': 'string, int[]'}.get(kind, 'int, int')
        projected = {'MapDynamic': 'value as int', 'MapNested': 'value[0]'}.get(kind, 'value')
        expected = len(final) + sum(rank * value for rank, value in enumerate(final.values(), 1))
        add('map/' + name, kind, count, expected, '',
            f'const map: Map<{arguments}> = {{' + ', '.join(entries) + '}; '
            'let total = map.size; let rank = 0; '
            f'for (const value in map.values()) {{ rank++; total += rank * ({projected}); }} return total;')
    add('map/NaN_zero', 'MapNaN', 5, 28, '',
        'const nan = Math.pow(-1.0, 0.5); '
        'const map: Map<double, int> = {nan: 1, 0.0: 2, -0.0: 3, nan: 4, 1.0: 5}; '
        'if (map.get(nan) != 4 || map.get(0.0) != 3) { return 0; } '
        'let total = map.size; let rank = 0; '
        'for (const value in map.values()) { rank++; total += rank * value; } return total;')
    return cases
