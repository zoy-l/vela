"""Generate offline native benchmark inputs through the production AOT CLI."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import time


def compile_workloads(compiler: Path, source: Path, exports: list[str], output: Path) -> dict:
    output.mkdir(parents=True, exist_ok=True)
    object_file = output / ("workload.obj" if compiler.suffix == ".exe" else "workload.o")
    arguments = [str(compiler), "build", str(source), "--emit=obj",
                 *["--export=" + name for name in exports], "-o", str(object_file)]
    started = time.perf_counter()
    subprocess.run(arguments, check=True)
    elapsed = time.perf_counter() - started
    manifest = json.loads(Path(str(object_file) + ".aot.json").read_text(encoding="utf-8"))
    metrics = {"command": arguments, "compilation_and_publication_seconds": elapsed,
               "object_bytes": object_file.stat().st_size, "runtime_abi": manifest["runtime_abi"],
               "toolchain_build": manifest["toolchain_build"], "target": manifest["target"],
               "optimization": manifest["optimization"]}
    (output / "build_metrics.json").write_text(json.dumps(metrics, indent=2) + "\n",
                                              encoding="utf-8", newline="\n")
    return manifest


def native_function_fields(manifest: dict, function: int) -> str:
    metrics = manifest["functions"][function]
    return (f", .function = {function}, .shadow_slots = {metrics['shadow_slots']}"
            f", .frame_bytes = {metrics['frame_bytes']}, .root_stores = {metrics['root_stores']}")


def aot_entries(cases: list[dict], manifest: dict) -> list[str]:
    indices = {entry["name"]: entry["function"] for entry in manifest["exports"]}
    header = ["#pragma once", "#include <array>",
              '#include "benchmarks/aot_benchmark_types.h"',
              'extern "C" const vela::runtime::NativeImage* ' + manifest["image"] + "() noexcept;",
              "namespace vela::benchmarks {",
              "inline const auto* const kAotBenchmarkImage = " + manifest["image"] + "();",
              "inline constexpr auto kAotBenchmarkCases = std::to_array<AotBenchmarkCase>({"]
    for case in cases:
        header.append("  {.name = " + json.dumps(case["name"]) + ", .kernel = AotKernel::" +
                      case["kernel"] + ", .size = " + str(case["size"]) + ", .expected = " +
                      str(case["expected"]) + ", .completion = AotCompletion::" + case["completion"] +
                      native_function_fields(manifest, indices[case["name"]]) + "},")
    header.extend(["});", "}  // namespace vela::benchmarks", ""])
    return header


def dasprofile_entries(cases: list[dict], manifest: dict) -> list[str]:
    indices = {entry["name"]: entry["function"] for entry in manifest["exports"]}
    header = ["#pragma once", "#include <array>",
              '#include "benchmarks/dasprofile/workloads.h"',
              'extern "C" const vela::runtime::NativeImage* ' + manifest["image"] + "() noexcept;",
              "namespace vela::benchmarks::dasprofile {",
              "inline const auto* const kImage = " + manifest["image"] + "();",
              "inline constexpr std::uint32_t kPrepare = " + str(indices["prepare"]) + ";",
              "inline constexpr auto kCases = std::to_array<Case>({"]
    for case in cases:
        header.append("  {.name = " + json.dumps(case["name"]) + ", .workload = Workload::" +
                      case["kernel"] + ", .size = " + str(case["size"]) +
                      ", .expected = " + str(case["expected"]) +
                      ", .increment = " + str(case.get("increment", 0)) +
                      native_function_fields(manifest, indices[case["name"]]) + "},")
    header.extend(["});", "}  // namespace vela::benchmarks::dasprofile", ""])
    return header


def generate(compiler: Path, source: Path, cases_path: Path, output: Path, suite: str) -> None:
    cases = json.loads(cases_path.read_text(encoding="utf-8"))
    exports = [case["name"] for case in cases]
    if suite == "dasprofile":
        exports.insert(0, "prepare")
    manifest = compile_workloads(compiler, source, exports, output)
    header = dasprofile_entries(cases, manifest) if suite == "dasprofile" else aot_entries(cases, manifest)
    (output / "entries.h").write_text("\n".join(header), encoding="utf-8", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--cases", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--suite", choices=("aot", "dasprofile"), required=True)
    arguments = parser.parse_args()
    generate(arguments.compiler.resolve(), arguments.source.resolve(), arguments.cases.resolve(),
             arguments.output.resolve(), arguments.suite)


if __name__ == "__main__":
    main()
