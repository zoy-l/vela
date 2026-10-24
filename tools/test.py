#!/usr/bin/env python3
"""Run the Vela bootstrap executable and its link test."""

from __future__ import annotations

import argparse
import subprocess
import sys

from tools.build import build
from tools.config import BUILD_VARIANTS, output_directory


def parse_arguments(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("test_arguments", nargs="*", metavar="VARIANT_OR_SELECTION")
    parsed = parser.parse_args(arguments)
    values = list(parsed.test_arguments)
    variant = "debug"
    if values and values[0] in BUILD_VARIANTS:
        variant = values.pop(0)
    selection = values[0] if values else "all"
    if len(values) > 1 or selection not in {"all", "list", "environment", "frontend"}:
        parser.error("expected list, all, environment, or frontend after VARIANT")
    return argparse.Namespace(variant=variant, selection=selection)


def run_tests(variant: str, selection: str) -> None:
    build(variant, "default")
    build(variant, "tests")
    output = output_directory(variant)

    if selection in {"all", "environment"}:
        test_executable = output / "vela_environment_tests.exe"
        subprocess.run([str(test_executable)], cwd=output, check=True)

    if selection in {"all", "frontend"}:
        frontend_tests = output / "vela_frontend_tests.exe"
        subprocess.run([str(frontend_tests)], cwd=output, check=True)

    version = subprocess.run(
        [str(output / "vela.exe"), "--version"],
        cwd=output,
        check=True,
        capture_output=True,
        text=True,
    )
    if version.stdout.strip() != "Vela 0.1.0":
        raise RuntimeError(
            f"Unexpected vela version output: {version.stdout!r}"
        )
    print(f"tests passed ({variant})")


def main(arguments: list[str] | None = None) -> int:
    parsed = parse_arguments(arguments)
    if parsed.selection == "list":
        print("environment")
        print("frontend")
        return 0
    run_tests(parsed.variant, parsed.selection)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"test failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
