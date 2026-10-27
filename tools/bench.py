#!/usr/bin/env python3
"""Build Vela Google Benchmark workloads; forward arguments after --."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys

from tools.benchmark_report import comparison_report, native_call_report
from tools.build import build
from tools.config import BUILD_VARIANTS, ROOT, executable_name, output_directory


def parse_arguments(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", choices=BUILD_VARIANTS, default="release")
    comparison = parser.add_mutually_exclusive_group()
    comparison.add_argument("--compare", action="store_true", help="Compare Vela, C++, LuaJIT and LuaJIT interpreter")
    comparison.add_argument("--native-compare", action="store_true", help="Compare VM native calls with direct and indirect C++ calls")
    parser.add_argument("--report", type=Path, help="Comparison JSON output (default: out/benchmarks/comparison.json)")
    values = list(sys.argv[1:] if arguments is None else arguments)
    separator = values.index("--") if "--" in values else len(values)
    parsed = parser.parse_args(values[:separator])
    parsed.benchmark_arguments = values[separator + 1:]
    comparing = parsed.compare or parsed.native_compare
    if parsed.report and not comparing:
        parser.error("--report requires --compare or --native-compare")
    if parsed.report and parsed.report.suffix != ".json":
        parser.error("--report must name a .json file")
    if comparing and parsed.variant != "release":
        parser.error("comparisons require the Release build")
    if comparing and any(value.startswith(("--benchmark_out=", "--benchmark_out_format="))
                              or value in {"--benchmark_out", "--benchmark_out_format"}
                              for value in parsed.benchmark_arguments):
        parser.error("use --report for comparison output")
    return parsed


def benchmark_option(arguments: list[str], name: str, default: str) -> str:
    value = default
    for index, argument in enumerate(arguments):
        if argument == f"--{name}":
            if index + 1 == len(arguments):
                raise ValueError(f"--{name} requires a value")
            value = arguments[index + 1]
        elif argument.startswith(f"--{name}="):
            value = argument.partition("=")[2]
    return value


def run(parsed: argparse.Namespace) -> None:
    build(parsed.variant, "benchmarks")
    executable = output_directory(parsed.variant) / executable_name("vela_benchmarks")
    arguments = list(parsed.benchmark_arguments)
    comparing = parsed.compare or parsed.native_compare
    if comparing:
        filename = "native_comparison.json" if parsed.native_compare else "comparison.json"
        report = (parsed.report or ROOT / "out" / "benchmarks" / filename).resolve()
        report.parent.mkdir(parents=True, exist_ok=True)
        # A failed invocation must never reuse an earlier successful report.
        report.unlink(missing_ok=True)
        report.with_suffix(".md").unlink(missing_ok=True)
        defaults = {
            "benchmark_filter": ("^native/" if parsed.native_compare else
                                 "^(compare/|first/(vm|luajit|luajit_interp)/|compile/(representative|luajit)/)"),
            "benchmark_min_time": "0.2s",
            "benchmark_repetitions": "7",
            "benchmark_enable_random_interleaving": "true",
        }
        for name, value in defaults.items():
            if not any(argument == f"--{name}" or argument.startswith(f"--{name}=") for argument in arguments):
                arguments.append(f"--{name}={value}")
        arguments.extend([f"--benchmark_out={report}", "--benchmark_out_format=json"])
        selected = subprocess.run(
            [str(executable), "--benchmark_list_tests=true",
             "--benchmark_filter=" + benchmark_option(arguments, "benchmark_filter", ".*")],
            cwd=ROOT, check=True, capture_output=True, text=True, encoding="utf-8")
        expected_cases = selected.stdout.splitlines()
        repetitions = int(benchmark_option(arguments, "benchmark_repetitions", "7"))
        if not expected_cases:
            raise ValueError("No benchmarks match the comparison filter")
        if repetitions < 1:
            raise ValueError("--benchmark_repetitions must be positive")
    subprocess.run(
        [str(executable), *arguments],
        cwd=ROOT,
        check=True,
    )
    if comparing:
        summarize = native_call_report if parsed.native_compare else comparison_report
        text = summarize(
            json.loads(report.read_text(encoding="utf-8")), expected_cases, repetitions)
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
