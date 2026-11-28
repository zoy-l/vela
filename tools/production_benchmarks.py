"""Generate checked production workloads and their AOT entry registry."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from aot_benchmarks import compile_workloads, native_image_declaration
from production_cases import DECLARATIONS, container_cases
from production_json_cases import DECLARATIONS as JSON_DECLARATIONS, json_cases
from production_async_cases import DECLARATIONS as ASYNC_DECLARATIONS, async_cases
from production_churn_cases import DECLARATIONS as CHURN_DECLARATIONS, churn_cases


def cases() -> list[dict]:
    return container_cases() + json_cases() + async_cases() + churn_cases()


def cpp_string(value: str) -> str:
    return 'R"json(' + value + ')json"'


def generate(compiler: Path, output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    records = cases()
    definitions = [DECLARATIONS, JSON_DECLARATIONS, ASYNC_DECLARATIONS, CHURN_DECLARATIONS,
                   "let jsonSource: string = '';",
                   'export func cleanup(): void { churnRing = []; churnSequence = 0; }']
    exports = ['cleanup']
    for index, case in enumerate(records):
        prepare = f'prepare{index}'
        entry = f'entry{index}'
        case['prepare_name'] = prepare
        case['entry_name'] = entry
        exports.extend([prepare, entry])
        size = 'size' if 'size' in case['setup'] else '_size'
        seed = 'seed' if 'seed' in case['setup'] else '_seed'
        signature = f'export async func {entry}(): Future<int>' if case.get('asynchronous') else f'export func {entry}(): int'
        definitions += [f'export func {prepare}({size}: int, {seed}: int): void {{ {case["setup"]} }}',
                        signature + ' { ' + case['body'] + ' }']
    source = output / 'workload.vela'
    source.write_text('\n'.join(definitions) + '\n', encoding='utf-8', newline='\n')
    manifest = compile_workloads(compiler, source, exports, output)
    indices = {record['name']: record['function'] for record in manifest['exports']}
    header = ['#pragma once', '#include <array>',
              '#include "benchmarks/production/contract.h"',
              '#include "runtime/native/native_image.h"', native_image_declaration(manifest),
              'namespace vela::benchmarks::production {',
              'inline const auto* const kImage = ' + manifest['image'] + '();',
              'inline constexpr auto kCases = std::to_array<Case>({']
    for case in records:
        header.append('  {.name = ' + json.dumps(case['name']) + ', .kind = Kind::' + case['kind'] +
                      ', .size = ' + str(case['size']) + ', .expected = ' + str(case['expected']) +
                      ', .prepare = ' + str(indices[case['prepare_name']]) +
                      ', .function = ' + str(indices[case['entry_name']]) +
                      ', .cleanup = ' + str(indices['cleanup']) +
                      ', .family = Family::' + {'containers': 'Containers', 'json': 'Json', 'async': 'Async', 'longrun': 'Churn'}[case['name'].split('/')[0]] +
                      ', .json = {' + ', '.join('.' + key + ' = ' +
                          (str(value) if isinstance(value, int) else cpp_string(value))
                          for key, value in case.get('json', {}).items()) + (',' if case.get('json') else '') + '},},')
    header += ['});', '}', '']
    (output / 'entries.h').write_text('\n'.join(header), encoding='utf-8', newline='\n')
    (output / 'cases.json').write_text(json.dumps(records, ensure_ascii=False, indent=2) + '\n',
                                     encoding='utf-8', newline='\n')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    arguments = parser.parse_args()
    generate(arguments.compiler.resolve(), arguments.output.resolve())


if __name__ == '__main__':
    main()
