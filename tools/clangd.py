#!/usr/bin/env python3
"""Generate the compile_commands.json files used by clangd."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from tools.config import BUILD_VARIANTS, ROOT, configure, toolchain_tools


def _compilation_database(output: Path) -> list[dict[str, object]]:
    tools = toolchain_tools()
    result = subprocess.run(
        [
            str(tools["ninja"]),
            "-C",
            str(output),
            "-t",
            "compdb",
            "cc",
            "cxx",
        ],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    try:
        database = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError("Ninja produced an invalid compilation database") from error
    if not isinstance(database, list) or any(
        not isinstance(entry, dict) for entry in database
    ):
        raise RuntimeError("Ninja produced an invalid compilation database root")
    return database


def _write_database(path: Path, database: list[dict[str, object]]) -> None:
    path.write_text(
        json.dumps(database, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def generate(variant: str) -> None:
    output = configure(variant)
    database = _compilation_database(output)
    output_database = output / "compile_commands.json"
    root_database = ROOT / "compile_commands.json"
    _write_database(output_database, database)
    _write_database(root_database, database)
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
