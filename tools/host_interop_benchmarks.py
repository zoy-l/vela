"""Generate the offline image and public wrappers for C++ interop benchmarks."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from aot_benchmarks import compile_workloads, native_image_declaration


def generate(compiler: Path, source: Path, output: Path) -> None:
    manifest = compile_workloads(compiler, source,
                                 ["scalar", "local", "callback", "object", "reentry"], output)
    module = manifest["instantiate"].removesuffix("_instantiate")
    interface = "workload.obj.h" if compiler.suffix == ".exe" else "workload.o.h"
    header = ["#pragma once", "#include <string_view>",
              '#include "' + interface + '" // IWYU pragma: export',
              '#include "runtime/native/native_image.h"',
              native_image_declaration(manifest),
              "namespace vela::benchmarks::interop {", f"namespace api = {module};",
              "inline const auto* const kImage = " + manifest["image"] + "();",
              "inline constexpr std::string_view kSource = " + json.dumps(source.as_posix()) + ";",
              "}", ""]
    (output / "entries.h").write_text("\n".join(header), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    generate(arguments.compiler.resolve(), arguments.source.resolve(), arguments.output.resolve())
