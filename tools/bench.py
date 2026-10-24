#!/usr/bin/env python3
"""Build Vela Google Benchmark workloads; forward arguments after --."""

from __future__ import annotations

import argparse
import subprocess
import sys

from tools.build import build
from tools.config import BUILD_VARIANTS, ROOT, executable_name, output_directory


def parse_arguments(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", choices=BUILD_VARIANTS, default="release")
    values = list(sys.argv[1:] if arguments is None else arguments)
    separator = values.index("--") if "--" in values else len(values)
    parsed = parser.parse_args(values[:separator])
    parsed.benchmark_arguments = values[separator + 1:]
    return parsed


def run(parsed: argparse.Namespace) -> None:
    build(parsed.variant, "benchmarks")
    executable = output_directory(parsed.variant) / executable_name("vela_benchmarks")
    subprocess.run(
        [str(executable), *parsed.benchmark_arguments],
        cwd=ROOT,
        check=True,
    )


def main(arguments: list[str] | None = None) -> int:
    parsed = parse_arguments(arguments)
    run(parsed)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"benchmark failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
