#!/usr/bin/env python3
"""Run the project-managed clang-tidy over Vela C++ sources."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from tools.clangd import generate as generate_compilation_database
from tools.config import BUILD_VARIANTS, ROOT, toolchain_tools


def parse_arguments(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path, metavar="PATH")
    parser.add_argument("--variant", choices=BUILD_VARIANTS, default="debug")
    parser.add_argument("--fix", action="store_true", help="Apply clang-tidy code fixes")
    return parser.parse_args(arguments)


def _database_sources() -> list[Path]:
    database_path = ROOT / "compile_commands.json"
    try:
        entries = json.loads(database_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot read compilation database: {database_path}") from error
    if not isinstance(entries, list):
        raise RuntimeError("compilation database root must be a list")

    sources: list[Path] = []
    seen: set[Path] = set()
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("file"), str):
            raise RuntimeError("compilation database contains an invalid source entry")
        directory = Path(entry.get("directory", ROOT))
        source = (directory / entry["file"]).resolve()
        if (
            source.suffix not in {".cc", ".cpp", ".cxx"}
            or source in seen
            or ROOT / "third_party" in source.parents
        ):
            continue
        seen.add(source)
        sources.append(source)
    return sources


def _path_sources(paths: list[Path]) -> list[Path]:
    sources: list[Path] = []
    for path in paths:
        candidate = (path if path.is_absolute() else Path.cwd() / path).resolve()
        if not candidate.exists():
            raise RuntimeError(f"lint path does not exist: {candidate}")
        if ROOT / "third_party" == candidate or ROOT / "third_party" in candidate.parents:
            raise RuntimeError(f"lint only checks project-owned C++ sources: {candidate}")
        if candidate.is_file():
            if candidate.suffix not in {".cc", ".cpp", ".cxx"}:
                raise RuntimeError(f"lint path is not a C++ source file: {candidate}")
            sources.append(candidate)
            continue
        sources.extend(
            source
            for source in sorted(candidate.rglob("*"))
            if source.is_file()
            and source.suffix in {".cc", ".cpp", ".cxx"}
            and ROOT / "third_party" not in source.parents
        )
    return sources


def run(parsed: argparse.Namespace) -> int:
    generate_compilation_database(parsed.variant)
    tools = toolchain_tools()
    sources = _path_sources(parsed.paths) if parsed.paths else _database_sources()
    if not sources:
        raise RuntimeError("no C++ source files were selected for lint")

    failed = 0
    for source in sources:
        command = [
            str(tools["clang_tidy"]), str(source), "-p", str(ROOT), "--quiet",
            "--warnings-as-errors=*",
        ]
        if parsed.fix:
            command.append("--fix")
        result = subprocess.run(
            command,
            cwd=ROOT,
            check=False,
        )
        if result.returncode != 0:
            failed += 1
    print(f"linted {len(sources)} source file(s); failures: {failed}")
    return 1 if failed else 0


def main(arguments: list[str] | None = None) -> int:
    parsed = parse_arguments(arguments)
    return run(parsed)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"lint failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
