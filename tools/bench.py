#!/usr/bin/env python3
"""Build Vela Google Benchmark workloads; forward arguments after --."""

from __future__ import annotations

import argparse
from collections.abc import Callable
from dataclasses import dataclass
import json
from pathlib import Path
import subprocess
import sys

from tools.benchmark_report import aot_comparison_report, comparison_report, jit_comparison_report, native_call_report
from tools.escape_benchmark_report import escape_comparison_report
from tools.gc_benchmark_report import gc_report
from tools.native_class_benchmark_report import native_class_comparison_report
from tools.reflection_benchmark_report import reflection_comparison_report
from tools.build import build
from tools.command_line import split_forwarded_arguments
from tools.config import BUILD_VARIANTS, ROOT, executable_name, output_directory


@dataclass(frozen=True)
class ReportMode:
    filename: str
    filter_pattern: str
    summarize: Callable[[dict, list[str], int], str]
    executable: str
    help: str


REPORT_MODES = {
    "compare": ReportMode(
        "comparison.json",
        "^(compare/|first/(jit|luajit|luajit_interp)/|compile/(representative|luajit)/)",
        comparison_report, "vela_benchmarks",
        "Compare Vela, C++, LuaJIT and LuaJIT interpreter"),
    "native_compare": ReportMode(
        "native_comparison.json", "^native/", native_call_report, "vela_aot_benchmarks",
        "Compare JIT/AOT native calls with direct and indirect C++ calls"),
    "native_class_compare": ReportMode(
        "native_class_comparison.json", "^native_class/", native_class_comparison_report,
        "vela_aot_benchmarks",
        "Compare generated class bindings across JIT, AOT and reloadable modes"),
    "aot_compare": ReportMode(
        "aot_comparison.json", "^aot/", aot_comparison_report, "vela_aot_benchmarks",
        "Compare offline AOT and C++ workloads"),
    "jit_compare": ReportMode(
        "jit_comparison.json", "^(aot/(native|cpp)/|hot/steady/)", jit_comparison_report,
        "vela_aot_benchmarks",
        "Compare reloadable baseline and optimized JIT with offline AOT and C++"),
    "escape_compare": ReportMode(
        "escape_comparison.json", "^(escape/|hot/(baseline|optimized)/virtual/)",
        escape_comparison_report, "vela_aot_benchmarks",
        "Compare heap, scalar replacement and private borrowing with virtual-frame reload"),
    "gc_report": ReportMode(
        "gc_report.json", "^gc/cohort/", gc_report, "vela_benchmarks",
        "Report Vela collector cycles, step latency and heap reclamation"),
    "reflection_compare": ReportMode(
        "reflection_comparison.json", "^reflection/", reflection_comparison_report, "vela_benchmarks",
        "Compare AOT/JIT reflection with ordinary/dynamic access and a hand-registered C++ reference"),
}


def parse_arguments(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", choices=BUILD_VARIANTS, default="release")
    comparison = parser.add_mutually_exclusive_group()
    for name, mode in REPORT_MODES.items():
        comparison.add_argument("--" + name.replace("_", "-"), action="store_const",
                                dest="report_mode", const=name, help=mode.help)
    parser.add_argument("--report", type=Path, help="Benchmark JSON report (default: out/benchmarks/<mode>.json)")
    options, forwarded = split_forwarded_arguments(arguments)
    parsed = parser.parse_args(options)
    parsed.benchmark_arguments = forwarded
    reporting = parsed.report_mode is not None
    if parsed.report and not reporting:
        parser.error("--report requires a report mode")
    if parsed.report and parsed.report.suffix != ".json":
        parser.error("--report must name a .json file")
    if reporting and parsed.variant != "release":
        parser.error("reports require the Release build")
    if reporting and any(value.startswith(("--benchmark_out=", "--benchmark_out_format="))
                              or value in {"--benchmark_out", "--benchmark_out_format"}
                              for value in parsed.benchmark_arguments):
        parser.error("use --report for report output")
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
    mode = REPORT_MODES.get(parsed.report_mode)
    name = mode.executable if mode else "vela_benchmarks"
    executable = output_directory(parsed.variant) / executable_name(name)
    arguments = list(parsed.benchmark_arguments)
    if mode:
        report = (parsed.report or ROOT / "out" / "benchmarks" / mode.filename).resolve()
        report.parent.mkdir(parents=True, exist_ok=True)
        # A failed invocation must never reuse an earlier successful report.
        report.unlink(missing_ok=True)
        report.with_suffix(".md").unlink(missing_ok=True)
        defaults = {
            "benchmark_filter": mode.filter_pattern,
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
            raise ValueError("No benchmarks match the report filter")
        if repetitions < 1:
            raise ValueError("--benchmark_repetitions must be positive")
    subprocess.run(
        [str(executable), *arguments],
        cwd=ROOT,
        check=True,
    )
    if mode:
        text = mode.summarize(
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
