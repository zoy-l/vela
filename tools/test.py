#!/usr/bin/env python3
"""Run Vela GoogleTest suites; forward arguments after -- to GoogleTest."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

from tools.build import build
from tools.command_line import split_forwarded_arguments
from tools.config import BUILD_VARIANTS, ROOT, executable_name


TEST_EXECUTABLES = {
    "base": "base_tests",
    "benchmarks": "vela_benchmark_tests",
    "frontend": "vela_frontend_tests",
    "ir": "vela_ir_tests",
    "aot": "vela_aot_tests",
    "jit": "vela_jit_tests",
    "compiler": "vela_compiler_tests",
    "runtime": "runtime_tests",
}


def parse_arguments(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("test_arguments", nargs="*", metavar="VARIANT_OR_SELECTION")
    options, gtest_arguments = split_forwarded_arguments(arguments)
    parsed = parser.parse_args(options)
    values = parsed.test_arguments
    variant = "debug"
    if values and values[0] in BUILD_VARIANTS:
        variant = values.pop(0)
    selection = values[0] if values else "all"
    if len(values) > 1 or selection not in {"all", "list", "tooling", *TEST_EXECUTABLES}:
        parser.error("expected list, all, or a test name after VARIANT")
    return argparse.Namespace(
        variant=variant, selection=selection, gtest_arguments=gtest_arguments
    )


def run_tests(variant: str, selection: str, gtest_arguments: list[str]) -> None:
    run_tooling = selection in {"all", "tooling"}
    targets = ["tooling_tests" if run_tooling else "default"]
    if selection != "tooling":
        targets.append("tests")
    output = build(variant, *targets)

    failed: list[str] = []
    for name, executable in TEST_EXECUTABLES.items():
        if selection in {"all", name}:
            result = subprocess.run(
                [str(output / executable_name(executable)), *gtest_arguments],
                cwd=output,
                check=False,
            )
            if result.returncode != 0:
                failed.append(name)

    if failed:
        raise RuntimeError(f"GoogleTest suites failed: {', '.join(failed)}")

    if run_tooling:
        environment = os.environ.copy()
        environment["VELA_TEST_EXECUTABLE"] = str(output / executable_name("vela"))
        subprocess.run([sys.executable, "-m", "unittest", "discover",
                        "-s", "tools", "-t", ".", "-p", "*_unittest.py"],
                       cwd=ROOT, env=environment, check=True)

    subprocess.run(
        [str(output / executable_name("vela")), "--version"],
        cwd=output,
        check=True,
    )
    print(f"test command completed ({variant})")


def main(arguments: list[str] | None = None) -> int:
    parsed = parse_arguments(arguments)
    if parsed.selection == "list":
        print("\n".join((*TEST_EXECUTABLES, "tooling")))
        return 0
    run_tests(parsed.variant, parsed.selection, parsed.gtest_arguments)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"test failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
