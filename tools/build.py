#!/usr/bin/env python3
"""Configure and build Vela targets with the project Ninja."""

from __future__ import annotations

import argparse
import subprocess
import sys

from tools.config import BUILD_VARIANTS, add_compiler_wrapper_argument, configure, toolchain_tools


ROOT_TARGETS = {"default", "tests", "benchmarks"}


def parse_arguments(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_arguments", nargs="*", metavar="VARIANT_OR_TARGET")
    add_compiler_wrapper_argument(parser)
    parsed = parser.parse_args(arguments)
    values = list(parsed.build_arguments)
    variant = "debug"
    if values and values[0] in BUILD_VARIANTS:
        variant = values.pop(0)
    if len(values) > 1:
        parser.error("expected at most one target after VARIANT")
    target = values[0] if values else "default"
    if target not in ROOT_TARGETS:
        parser.error(f"unsupported build target: {target}")
    return argparse.Namespace(variant=variant, target=target, cc_wrapper=parsed.cc_wrapper)


def build(variant: str, target: str, *, cc_wrapper: str = "sccache") -> None:
    output = configure(variant, cc_wrapper=cc_wrapper)
    tools = toolchain_tools()
    ninja_target = ":" + target
    subprocess.run(
        [str(tools["ninja"]), "-C", str(output), ninja_target],
        cwd=output,
        check=True,
    )


def main(arguments: list[str] | None = None) -> int:
    parsed = parse_arguments(arguments)
    build(parsed.variant, parsed.target, cc_wrapper=parsed.cc_wrapper)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"build failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
