"""Compile the representative standard API scripts through the production AOT CLI."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from aot_benchmarks import compile_workloads, native_function_fields, native_image_declaration


def generate(compiler: Path, output: Path, sources: list[Path], cases_path: Path,
             entry_path: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    cases = json.loads(cases_path.read_text(encoding="utf-8"))
    table = ["#pragma once", "#include <array>",
             '#include "benchmarks/workload.h"',
             '#include "benchmarks/standard_api_workloads.h"',
             "namespace vela::benchmarks {",
             "inline constexpr auto kStandardApiWorkloads = std::to_array<Workload>({"]
    for case in cases:
        problems = ", ".join("{.size = " + str(size) + ", .expected = " + str(expected) + "}"
                             for size, expected in case["problems"])
        table.append("  {.name = " + json.dumps(case["name"]) + ", .native = " + case["native"] +
                     ", .problems = {{" + problems + "}}},")
    table.extend(["});", "}  // namespace vela::benchmarks", ""])
    (output / "workloads.h").write_text("\n".join(table), encoding="utf-8", newline="\n")
    definitions = ["import 'input' as data;",
                   "export func prepare(size: int): void { data.prepare(size); }"]
    exports = ["prepare"]
    entry_source = entry_path.read_text(encoding="utf-8")
    for index, source in enumerate(sources):
        text = source.read_text(encoding="utf-8")
        if source.stem == "input":
            (output / source.name).write_text(text, encoding="utf-8", newline="\n")
            continue
        (output / source.name).write_text(
            text + "\n" + entry_source,
            encoding="utf-8", newline="\n")
        name = source.stem
        definitions.insert(0, f"import '{name}' as workload{index};")
        definitions.append(f"export func {name}(): int {{ return workload{index}.benchmarkEntry(); }}")
        exports.append(name)
    source = output / "main.vela"
    source.write_text("\n".join(definitions) + "\n", encoding="utf-8", newline="\n")
    manifest = compile_workloads(compiler, source, exports, output)
    header = ["#pragma once", "#include <array>", "#include <cstdint>", "#include <string_view>",
              '#include "runtime/native/native_image.h"',
              native_image_declaration(manifest),
              "namespace vela::benchmarks::standard_api {",
              "inline const auto* const kImage = " + manifest["image"] + "();",
              "struct Entry final { std::string_view name; std::uint32_t function; "
              "std::size_t shadow_slots; std::size_t frame_bytes; std::size_t root_stores; };",
              "inline constexpr auto kEntries = std::to_array<Entry>({"]
    for entry in manifest["exports"]:
        if entry["name"] != "prepare":
            header.append('{.name = "' + entry["name"] + '"' +
                          native_function_fields(manifest, entry["function"]) + "},")
    header.extend(["});", "}", ""])
    (output / "entries.h").write_text("\n".join(header), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cases", type=Path, required=True)
    parser.add_argument("--entry", type=Path, required=True)
    parser.add_argument("sources", nargs="+", type=Path)
    arguments = parser.parse_args()
    generate(arguments.compiler.resolve(), arguments.output.resolve(),
             [source.resolve() for source in arguments.sources], arguments.cases.resolve(),
             arguments.entry.resolve())
