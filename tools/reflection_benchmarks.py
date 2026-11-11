"""Generate shared reflection workloads and compile their offline native images."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from string import Template

from aot_benchmarks import compile_workloads, native_image_declaration


WORKLOADS = Path(__file__).resolve().parent.parent / "benchmarks/reflection"
QUERY_SIZES = ((64, 8), (1024, 8), (1024, 64), (1024, 256), (1024, 0))


def programs() -> list[dict]:
    plain = (WORKLOADS / "plain.vela").read_text(encoding="utf-8")
    reflected = "[Reflect]\n" + plain + (WORKLOADS / "operations.vela").read_text(encoding="utf-8")
    cases = json.loads((WORKLOADS / "cases.json").read_text(encoding="utf-8"))
    result = []
    for name, text in (("plain", plain), ("registered", "[Reflect]\n" + plain), ("reflected", reflected)):
        selected = [dict(case, operations=128, registered=name != "plain", query=None)
                    for case in cases if case["program"] == name]
        result.append({"name": name, "sources": {"main.vela": text}, "cases": selected})
    template = Template((WORKLOADS / "query.vela.in").read_text(encoding="utf-8"))
    for background, matches in QUERY_SIZES:
        unused = "\n".join(f"  static unused{index:04d}(): int {{ return 0; }}"
                           for index in range(background))
        methods = "\n".join(f"  [tags.Button(index: {index}, label: 'button')]\n"
                            f"  static action{index:04d}(): int {{ return {index}; }}"
                            for index in range(matches))
        sources = {
            "main.vela": template.substitute(background=unused, matches=matches),
            "attributes.vela": "export class Button extends Reflect { index: int = 0; label: string = ''; }\n",
            "targets.vela": "import 'attributes' as tags;\nexport class Actions {\n" + methods + "\n}\n",
        }
        selected = []
        modes = ("all", "module", "missing") if (background, matches) == (1024, 8) else ("all",)
        for mode in modes:
            selected.append({
                "path": f"query/{mode}/background:{background}/matches:{matches}",
                "entry": "query" + mode.title(), "operations": 8,
                "expected": 0 if mode == "missing" else 8 * matches, "registered": True,
                "query": {"background": background, "matches": matches},
            })
        result.append({"name": f"query_{background}_{matches}", "sources": sources, "cases": selected})
    return result


def generate(compiler: Path, output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    header = ["#pragma once", "#include <array>", '#include "benchmarks/reflection_benchmark.h"']
    compiled = []
    for program in programs():
        name = program["name"]
        directory = output / name
        directory.mkdir(parents=True, exist_ok=True)
        for path, text in program["sources"].items():
            (directory / path).write_text(text, encoding="utf-8", newline="\n")
        exports = [case["entry"] for case in program["cases"]]
        if program["cases"][0]["query"] is not None:
            exports.append("verify")
        manifest = compile_workloads(compiler, directory / "main.vela", exports, directory)
        object_file = directory / next(file["path"] for file in manifest["files"]
                                       if file["role"] == "binary")
        compiled.append((program, manifest, object_file.stat().st_size))
        header.append(native_image_declaration(manifest))
    header.append("namespace vela::benchmarks::reflection_benchmark {")
    for program, _, _ in compiled:
        name = program["name"]
        header.append(f"inline constexpr auto kSources_{name} = std::to_array<Source>({{")
        for path, text in program["sources"].items():
            header.append("{.path = " + json.dumps(path) + ", .text = " + json.dumps(text) + "},")
        header += ["});", f"inline constexpr auto kCases_{name} = std::to_array<ScriptRun>({{"]
        for case in program["cases"]:
            query = case["query"]
            query_text = ("std::nullopt" if query is None else
                          "QuerySize{.background = " + str(query["background"]) +
                          ", .matches = " + str(query["matches"]) + "}")
            registration = "Enabled" if case["registered"] else "Disabled"
            header.append("{.path = " + json.dumps(case["path"]) + ", .entry = " + json.dumps(case["entry"]) +
                          ", .operations = " + str(case["operations"]) + ", .expected = " + str(case["expected"]) +
                          ", .registration = Registration::" + registration + ", .query = " + query_text + "},")
        header.append("});")
    header.append("inline constexpr auto kPrograms = std::to_array<Program>({")
    for program, manifest, object_bytes in compiled:
        name = program["name"]
        header.append(f"Program(kSources_{name}, kCases_{name}, " +
                      manifest["image"] + ", " + str(object_bytes) + "),")
    header += ["});", "}", ""]
    (output / "entries.h").write_text("\n".join(header), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    generate(arguments.compiler.resolve(), arguments.output.resolve())
