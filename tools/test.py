#!/usr/bin/env python3
"""Run Vela GoogleTest suites; forward arguments after -- to GoogleTest."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

from tools.build import build
from tools.config import BUILD_VARIANTS, executable_name, output_directory


TEST_EXECUTABLES = {
    "base": "base_tests",
    "environment": "vela_environment_tests",
    "frontend": "vela_frontend_tests",
    "ir": "vela_ir_tests",
    "aot": "vela_aot_tests",
    "compiler": "vela_compiler_tests",
    "runtime": "runtime_tests",
    "vm": "vm_tests",
}


def parse_arguments(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("test_arguments", nargs="*", metavar="VARIANT_OR_SELECTION")
    values = list(sys.argv[1:] if arguments is None else arguments)
    separator = values.index("--") if "--" in values else len(values)
    parsed = parser.parse_args(values[:separator])
    gtest_arguments = values[separator + 1:]
    values = list(parsed.test_arguments)
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
    build(variant, "default")
    if selection != "tooling":
        build(variant, "tests")
    output = output_directory(variant)

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

    if selection in {"all", "tooling"}:
        environment = os.environ.copy()
        environment["VELA_TEST_EXECUTABLE"] = str(output / executable_name("vela"))
        subprocess.run([sys.executable, "-m", "unittest", "tools.tooling_unittest", "tools.editor_unittest", "tools.command_unittest", "tools.bench_unittest", "tools.toolchain_unittest", "tools.unicode_case_unittest", "tools.aot_unittest"],
                       cwd=output.parent.parent, env=environment, check=True)

    version = subprocess.run(
        [str(output / executable_name("vela")), "--version"],
        cwd=output,
        check=True,
        capture_output=True,
        text=True,
    )
    if version.stdout.strip() != "Vela 0.1.0":
        raise RuntimeError(
            f"Unexpected vela version output: {version.stdout!r}"
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
