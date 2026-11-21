"""Generate shared JIT/AOT workloads for native free-function call benchmarks."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from aot_benchmarks import compile_workloads, native_image_declaration


BATCH_SIZES = (64, 4096)


def string_workload(name: str, text: str) -> tuple[str, str, str, str, str, str]:
    return (name, "StringBorrow", "int", f"sum += hostStringBorrow('{text}');",
            f"sum += {len(text) + ord(text[0])};", text)


WORKLOADS = (
    ("empty", "Empty", "int", "hostEmpty(); sum += index + 1;", "sum += index + 1;", ""),
    ("integer", "Integer", "int", "sum += hostInteger(index);", "sum += index + 1;", ""),
    ("double", "Double", "double", "sum += hostDouble(index.toDouble());",
     "sum += index.toDouble() + 0.5;", ""),
    string_workload("string_borrow", "vela-native"),
    ("boolean", "Boolean", "int", "sum += (hostBoolean((index & 1) == 0) ? 1 : 0);",
     "sum += index & 1;", ""),
    string_workload("string_borrow_96", "x" * 96),
    string_workload("string_borrow_4096", "x" * 4096),
)
DECLARATIONS = """external func hostEmpty(): void = 'callbench.empty';
external func hostInteger(value: int): int = 'callbench.integer';
external func hostDouble(value: double): double = 'callbench.double';
external func hostStringBorrow(text: string): int = 'callbench.string_borrow';
"""


def generate(compiler: Path, output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    definitions = [DECLARATIONS]
    cases = []
    exports = []
    for name, kind, result, native, control, text in WORKLOADS:
        if kind == "Boolean":
            # Append the new binding after the original kernels so their function indices stay stable.
            definitions.append("external func hostBoolean(value: bool): bool = 'callbench.boolean';")
        for count in BATCH_SIZES:
            names = []
            for path, body in (("native", native), ("control", control)):
                entry = f"{path}_{name}_{count}"
                names.append(entry)
                exports.append(entry)
                zero = "0.0" if result == "double" else "0"
                definitions.append(f"export func {entry}(): {result} {{ let sum = {zero}; "
                                   f"for (let index = 0; index < {count}; index++) {{ {body} }} "
                                   "return sum; }\n")
            cases.append((name, kind, count, *names, text))
    source = output / "workloads.vela"
    source.write_text("\n".join(definitions), encoding="utf-8", newline="\n")
    manifest = compile_workloads(compiler, source, exports, output)
    header = ["#pragma once", "#include <array>", "#include <cstdint>",
              "#include <string_view>", '#include "runtime/native/native_image.h"',
              native_image_declaration(manifest),
              "namespace vela::benchmarks::native_call {",
              "inline const auto* const kImage = " + manifest["image"] + "();",
              "inline constexpr std::string_view kSource = " + json.dumps(source.as_posix(), ensure_ascii=False) + ";",
              "enum class Workload : std::uint8_t { "
              + ", ".join(dict.fromkeys(workload[1] for workload in WORKLOADS)) + " };",
              "struct Case final { std::string_view name; Workload workload; std::int64_t count; "
              "std::string_view native; std::string_view control; std::string_view text; };",
              "inline constexpr auto kCases = std::to_array<Case>({"]
    for name, kind, count, native, control, text in cases:
        header.append("{.name = " + json.dumps(name) + ", .workload = Workload::" + kind
                      + ", .count = " + str(count) + ", .native = " + json.dumps(native)
                      + ", .control = " + json.dumps(control) + ", .text = " + json.dumps(text)
                      + "},")
    header.extend(["});", "}", ""])
    (output / "entries.h").write_text("\n".join(header), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    generate(arguments.compiler.resolve(), arguments.output.resolve())
