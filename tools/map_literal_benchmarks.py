"""Generate paired Map literal/API workloads and compile their offline image."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from aot_benchmarks import compile_workloads, native_function_fields


SHAPES = {"empty": 0, "small": 4, "large": 128, "duplicates": 128,
          "dynamic": 32, "nested": 32}


def workload(shape: str, form: str, name: str) -> tuple[str, int]:
    count = SHAPES[shape]
    types = {"dynamic": "dynamic, dynamic", "nested": "string, int[]"}
    arguments = types.get(shape, "int, int")
    entries: list[tuple[str, str]] = []
    values: dict[str, int] = {}
    for index in range(count):
        key = str(index % 8 if shape == "duplicates" else index)
        if shape == "dynamic" and index % 2:
            key += ".0"
        if shape == "nested":
            key = "'key" + key + "'"
        value = str(index + 1)
        entries.append((key, f"[{value}]" if shape == "nested" else value))
        values[key] = index + 1
    if form == "literal":
        construction = "{" + ", ".join(f"{key}: {value}" for key, value in entries) + "}"
        body = f"const map: Map<{arguments}> = {construction};\n"
    else:
        body = f"const map = Map<{arguments}>();\n"
        body += "".join(f"map.set({key}, {value});\n" for key, value in entries)
    projected = {"dynamic": "value as int", "nested": "value[0]"}.get(shape, "value")
    body += f"let total = map.size;\nfor (const [key, value] in map) {{ total += {projected}; }}\n"
    return f"export func {name}(): int {{\n{body}return total;\n}}\n", len(values) + sum(values.values())


def generate(compiler: Path, output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    cases = []
    for shape in SHAPES:
        for form in ("literal", "api"):
            name = f"{shape}_{form}"
            source, expected = workload(shape, form, name)
            standalone, _ = workload(shape, form, "main")
            cases.append({"name": name, "suffix": f"{shape}/{form}", "source": source,
                          "standalone": standalone, "expected": expected})
    source_file = output / "workload.vela"
    source_file.write_text("\n".join(case["source"] for case in cases),
                           encoding="utf-8", newline="\n")
    manifest = compile_workloads(compiler, source_file, [case["name"] for case in cases], output)
    indices = {entry["name"]: entry["function"] for entry in manifest["exports"]}
    header = ["#pragma once", "#include <array>",
              '#include "benchmarks/map_literal_benchmark.h"',
              'extern "C" const vela::runtime::NativeImage* ' + manifest["image"] + "() noexcept;",
              "namespace vela::benchmarks {",
              "inline const auto* const kMapLiteralImage = " + manifest["image"] + "();",
              "inline constexpr auto kMapLiteralCases = std::to_array<MapLiteralCase>({"]
    for case in cases:
        header.append("  {.name = " + json.dumps(case["suffix"]) +
                      ", .source = " + json.dumps(case["standalone"]) +
                      ", .expected = " + str(case["expected"]) +
                      native_function_fields(manifest, indices[case["name"]]) + "},")
    header.extend(["});", "}  // namespace vela::benchmarks", ""])
    (output / "entries.h").write_text("\n".join(header), encoding="utf-8", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    generate(arguments.compiler.resolve(), arguments.output.resolve())


if __name__ == "__main__":
    main()
