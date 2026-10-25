#!/usr/bin/env python3
"""Build Vela Google Benchmark workloads; forward arguments after --."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys

from tools.benchmark_report import comparison_report
from tools.build import build
from tools.config import BUILD_VARIANTS, ROOT, executable_name, output_directory


def parse_arguments(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", choices=BUILD_VARIANTS, default="release")
    parser.add_argument("--compare", action="store_true", help="Run paired Vela/C++ workloads and summarize ratios")
    parser.add_argument("--report", type=Path, help="Comparison JSON output (default: out/benchmarks/comparison.json)")
    values = list(sys.argv[1:] if arguments is None else arguments)
    separator = values.index("--") if "--" in values else len(values)
    parsed = parser.parse_args(values[:separator])
    parsed.benchmark_arguments = values[separator + 1:]
    if parsed.report and not parsed.compare:
        parser.error("--report requires --compare")
    if parsed.report and parsed.report.suffix != ".json":
        parser.error("--report must name a .json file")
    if parsed.compare and parsed.variant != "release":
        parser.error("--compare requires the Release build")
    if parsed.compare and any(value.startswith(("--benchmark_out=", "--benchmark_out_format="))
                              or value in {"--benchmark_out", "--benchmark_out_format"}
                              for value in parsed.benchmark_arguments):
        parser.error("use --report for comparison output")
    return parsed


def run(parsed: argparse.Namespace) -> None:
    build(parsed.variant, "benchmarks")
    executable = output_directory(parsed.variant) / executable_name("vela_benchmarks")
    arguments = list(parsed.benchmark_arguments)
    if parsed.compare:
        report = (parsed.report or ROOT / "out" / "benchmarks" / "comparison.json").resolve()
        report.parent.mkdir(parents=True, exist_ok=True)
        # A failed invocation must never reuse an earlier successful report.
        report.unlink(missing_ok=True)
        report.with_suffix(".md").unlink(missing_ok=True)
        defaults = {
            "benchmark_filter": "^(compare/|compile/representative/)",
            "benchmark_min_time": "0.2s",
            "benchmark_repetitions": "7",
            "benchmark_enable_random_interleaving": "true",
        }
        for name, value in defaults.items():
            if not any(argument == f"--{name}" or argument.startswith(f"--{name}=") for argument in arguments):
                arguments.append(f"--{name}={value}")
        arguments.extend([f"--benchmark_out={report}", "--benchmark_out_format=json"])
    subprocess.run(
        [str(executable), *arguments],
        cwd=ROOT,
        check=True,
    )
    if parsed.compare:
        text = comparison_report(json.loads(report.read_text(encoding="utf-8")))
        report.with_suffix(".md").write_text(text, encoding="utf-8", newline="\n")
        print(text)
        print(f"Raw samples: {report}")


def main(arguments: list[str] | None = None) -> int:
    parsed = parse_arguments(arguments)
    run(parsed)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"benchmark failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
