"""Sync pinned UCD inputs, or generate case tables offline for GN."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile
import urllib.request


ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / 'third_party/unicode_data'
MANIFEST = DATA / 'manifest.json'


def checked_bytes(path: Path, expected: str) -> bytes:
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError(f'UCD identity mismatch: {path}')
    return data


def sync(manifest: dict) -> None:
    directory = DATA / 'inputs'
    directory.mkdir(exist_ok=True)
    for name, spec in manifest['files'].items():
        target = directory / name
        if target.is_file() and hashlib.sha256(target.read_bytes()).hexdigest() == spec['sha256']:
            continue
        with urllib.request.urlopen(spec['url'], timeout=60) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != spec['sha256']:
            raise ValueError(f'UCD download identity mismatch: {name}')
        with tempfile.NamedTemporaryFile(dir=directory, delete=False) as output:
            output.write(data)
            temporary = Path(output.name)
        os.replace(temporary, target)


def codepoints(text: str) -> list[int]:
    points = [int(value, 16) for value in text.split()]
    if not 1 <= len(points) <= 3 or any(
        point > 0x10ffff or 0xd800 <= point <= 0xdfff for point in points
    ):
        raise ValueError(f'Unsupported case mapping: {text}')
    return points


def tables(manifest: dict) -> tuple[list, dict[str, list]]:
    version = manifest['unicode_version']
    checked_bytes(DATA / 'LICENSE', manifest['license_sha256'])
    makefile = (ROOT / 'third_party/utf8proc/data/Makefile').read_text(encoding='utf-8')
    actual = re.search(r'^UNICODE_VERSION=(\S+)$', makefile, re.M)
    if actual is None or actual.group(1) != version:
        raise ValueError('UCD version must match the pinned utf8proc version')
    inputs = {
        name: checked_bytes(DATA / 'inputs' / name, spec['sha256']).decode('utf-8')
        for name, spec in manifest['files'].items()
    }
    for name in ['SpecialCasing.txt', 'DerivedCoreProperties.txt']:
        if f'-{version}.txt' not in inputs[name].splitlines()[0]:
            raise ValueError(f'UCD version mismatch: {name}')
    mappings = {}
    final_sigma = None
    for line in inputs['SpecialCasing.txt'].splitlines():
        fields = [field.strip() for field in line.split('#', 1)[0].split(';')]
        if len(fields) < 5:
            continue
        if fields[4]:
            if fields[4] == 'Final_Sigma':
                final_sigma = (int(fields[0], 16), codepoints(fields[1]))
            elif not set(fields[4].split()) & {'tr', 'az', 'lt'}:
                raise ValueError(f'Unimplemented default casing condition: {fields[4]}')
            continue
        point = int(fields[0], 16)
        if point in mappings:
            raise ValueError(f'Duplicate SpecialCasing entry: {point:x}')
        mappings[point] = (codepoints(fields[1]), codepoints(fields[3]))
    if final_sigma != (0x3a3, [0x3c2]):
        raise ValueError('Unexpected Final_Sigma mapping')
    properties: dict[str, list] = {'Cased': [], 'Case_Ignorable': []}
    for line in inputs['DerivedCoreProperties.txt'].splitlines():
        fields = [field.strip() for field in line.split('#', 1)[0].split(';')]
        if len(fields) < 2 or fields[1] not in properties:
            continue
        bounds = fields[0].split('..')
        first, last = int(bounds[0], 16), int(bounds[-1], 16)
        ranges = properties[fields[1]]
        if first > last or last > 0x10ffff or (ranges and first <= ranges[-1][1]):
            raise ValueError(f'Invalid property range: {line}')
        if ranges and first == ranges[-1][1] + 1:
            ranges[-1] = (ranges[-1][0], last)
        else:
            ranges.append((first, last))
    if not mappings or not all(properties.values()):
        raise ValueError('Missing Unicode casing data')
    return sorted(mappings.items()), properties


def generate(manifest: dict) -> str:
    mappings, properties = tables(manifest)
    lines = [
        '// 由 tools/unicode_case.py 从固定 UCD 输入生成，不要手动修改。',
        '#pragma once', '#include <array>', '#include <cstdint>', '#include <string_view>',
        'namespace vela::runtime::internal {',
        f'inline constexpr std::string_view kCaseUnicodeVersion = "{manifest["unicode_version"]}";',
        'struct CaseMapping {', '  std::int32_t point;',
        '  std::array<std::int32_t, 3> lower;', '  std::array<std::int32_t, 3> upper;',
        '  std::uint8_t lower_size;', '  std::uint8_t upper_size;', '};',
        'struct CaseRange { std::int32_t first; std::int32_t last; };',
        'inline constexpr auto kFullCaseMappings = std::to_array<CaseMapping>({',
    ]
    for point, (lower, upper) in mappings:
        array = lambda values: '{' + ', '.join(f'0x{n:x}' for n in values) + '}'
        lines += [f'  {{.point = 0x{point:x},',
                  f'   .lower = {array(lower)},', f'   .upper = {array(upper)},',
                  f'   .lower_size = {len(lower)}, .upper_size = {len(upper)},', '  },']
    lines.append('});')
    for prop, name in [('Cased', 'kCasedRanges'), ('Case_Ignorable', 'kCaseIgnorableRanges')]:
        ranges = properties[prop]
        lines.append(f'inline constexpr auto {name} = std::to_array<CaseRange>({{')
        lines += [f'  {{.first = 0x{first:x}, .last = 0x{last:x}}},' for first, last in ranges]
        lines.append('});')
    lines += ['}  // namespace vela::runtime::internal', '']
    return '\n'.join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sync', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text(encoding='utf-8'))
    if args.sync:
        sync(manifest)
    if args.output:
        content = generate(manifest)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(content, encoding='utf-8', newline='\n')
    elif not args.sync:
        parser.error('--sync or --output is required')


if __name__ == '__main__':
    main()
