"""Generate offline native benchmark inputs through the production AOT CLI."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import time


def generate(compiler: Path, source: Path, cases_path: Path, output: Path) -> None:
    cases = json.loads(cases_path.read_text(encoding="utf-8"))
    output.mkdir(parents=True, exist_ok=True)
    object_file = output / ("workload.obj" if compiler.suffix == ".exe" else "workload.o")
    arguments = [str(compiler), "build", str(source), "--emit=obj",
                 *["--export=" + case["name"] for case in cases], "-o", str(object_file)]
    started = time.perf_counter()
    subprocess.run(arguments, check=True)
    elapsed = time.perf_counter() - started
    manifest = json.loads(Path(str(object_file) + ".aot.json").read_text(encoding="utf-8"))
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
                      ", .function = " + str(indices[case["name"]]) +
                      ", .shadow_slots = " + str(manifest["functions"][indices[case["name"]]]["shadow_slots"]) +
                      ", .frame_bytes = " + str(manifest["functions"][indices[case["name"]]]["frame_bytes"]) +
                      ", .root_stores = " + str(manifest["functions"][indices[case["name"]]]["root_stores"]) + "},")
    header.extend(["});", "}  // namespace vela::benchmarks", ""])
    (output / "entries.h").write_text("\n".join(header), encoding="utf-8", newline="\n")
    metrics = {"command": arguments, "compilation_and_publication_seconds": elapsed,
               "object_bytes": object_file.stat().st_size, "runtime_abi": manifest["runtime_abi"],
               "toolchain_build": manifest["toolchain_build"], "target": manifest["target"],
               "optimization": manifest["optimization"]}
    (output / "build_metrics.json").write_text(json.dumps(metrics, indent=2) + "\n",
                                              encoding="utf-8", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--cases", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    generate(arguments.compiler.resolve(), arguments.source.resolve(), arguments.cases.resolve(),
             arguments.output.resolve())


if __name__ == "__main__":
    main()
