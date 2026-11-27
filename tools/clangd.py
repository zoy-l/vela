#!/usr/bin/env python3
"""Generate the compile_commands.json files used by clangd."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys

from tools.config import BUILD_VARIANTS, ROOT, configure


def generate(variant: str) -> None:
    output_database = configure(variant) / "compile_commands.json"
    contents = output_database.read_bytes()
    try:
        database = json.loads(contents)
    except json.JSONDecodeError as error:
        raise RuntimeError("GN produced an invalid compilation database") from error
    if not isinstance(database, list) or any(
        not isinstance(entry, dict) for entry in database
    ):
        raise RuntimeError("GN produced an invalid compilation database root")
    root_database = ROOT / "compile_commands.json"
    root_database.write_bytes(contents)
    print(f"clangd compilation database: {root_database.relative_to(ROOT)}")
    print(f"entries: {len(database)} ({output_database.relative_to(ROOT)})")


def parse_arguments(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("variant", choices=BUILD_VARIANTS, default="debug", nargs="?")
    return parser.parse_args(arguments)


def main(arguments: list[str] | None = None) -> int:
    parsed = parse_arguments(arguments)
    generate(parsed.variant)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"clangd failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
