#!/usr/bin/env python3
"""Configure and build Vela targets with the project Ninja."""

from __future__ import annotations

import argparse
from collections.abc import Iterator
import json
from pathlib import Path
import subprocess
import sys

from tools.config import BUILD_VARIANTS, ROOT, add_compiler_wrapper_argument, build_tool, configure


ROOT_TARGETS = {"default", "tests", "benchmarks", "embedding"}


def parse_arguments(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_arguments", nargs="*", metavar="VARIANT_OR_TARGET")
    add_compiler_wrapper_argument(parser)
    parsed = parser.parse_args(arguments)
    values = parsed.build_arguments
    variant = "debug"
    if values and values[0] in BUILD_VARIANTS:
        variant = values.pop(0)
    if len(values) > 1:
        parser.error("expected at most one target after VARIANT")
    target = values[0] if values else "default"
    if target not in ROOT_TARGETS:
        parser.error(f"unsupported build target: {target}")
    return argparse.Namespace(variant=variant, target=target, cc_wrapper=parsed.cc_wrapper)


def describe_targets(output: Path) -> dict[str, dict]:
    result = subprocess.run([str(build_tool("gn")), "desc", str(output), "//*", "--format=json"],
                            cwd=ROOT, check=True, capture_output=True, text=True, encoding="utf-8")
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError("GN produced an invalid target description") from error


def target_closure(targets: dict[str, dict], *roots: str) -> Iterator[dict]:
    pending = list(roots)
    visited = set()
    while pending:
        label = pending.pop()
        if label in visited:
            continue
        visited.add(label)
        target = targets[label]
        pending.extend(target.get("deps", []))
        pending.extend(target.get("public_deps", []))
        yield target


def build(variant: str, target: str, *additional_targets: str, cc_wrapper: str = "sccache") -> Path:
    output = configure(variant, cc_wrapper=cc_wrapper)
    ninja_targets = [":" + name for name in (target, *additional_targets)]
    subprocess.run(
        [str(build_tool("ninja")), "-C", str(output), *ninja_targets],
        cwd=ROOT,
        check=True,
    )
    return output


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
