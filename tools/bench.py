#!/usr/bin/env python3
"""Build and run Vela benchmark workloads with the project-local toolchain."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from tools.build import build
from tools.config import BUILD_VARIANTS, executable_name, output_directory


ROOT = Path(__file__).resolve().parents[1]


def parse_positive_integer(value: str) -> int:
    try:
        result = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be a positive integer") from error
    if result <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return result


def parse_arguments(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="*", type=Path, metavar="FILE")
    parser.add_argument("--iterations", type=parse_positive_integer, default=1000)
    parser.add_argument("--variant", choices=BUILD_VARIANTS, default="release")
    return parser.parse_args(arguments)


def resolve_sources(files: list[Path]) -> list[Path]:
    candidates = files or sorted((ROOT / "benchmarks").glob("*.vela"))
    if not candidates:
        raise RuntimeError("no benchmark source files were found")

    sources: list[Path] = []
    for candidate in candidates:
        source = candidate if candidate.is_absolute() else Path.cwd() / candidate
        source = source.resolve()
        if not source.is_file():
            raise RuntimeError(f"benchmark source does not exist: {source}")
        sources.append(source)
    return sources


def run(parsed: argparse.Namespace) -> None:
    sources = resolve_sources(parsed.files)
    build(parsed.variant, "default")
    executable = output_directory(parsed.variant) / executable_name("vela")
    for source in sources:
        subprocess.run(
            [str(executable), "bench", str(source), str(parsed.iterations)],
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
