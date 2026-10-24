#!/usr/bin/env python3
"""Run the project-managed clang-tidy over Vela C++ sources and headers."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from tools.clangd import generate as generate_compilation_database
from tools.config import BUILD_VARIANTS, ROOT, output_directory, toolchain_tools


CPP_SUFFIXES = {".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx"}


def parse_arguments(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path, metavar="PATH")
    parser.add_argument("--variant", choices=BUILD_VARIANTS, default="debug")
    parser.add_argument("--fix", action="store_true", help="Apply clang-tidy code fixes")
    return parser.parse_args(arguments)


def _is_project_file(path: Path) -> bool:
    return (
        path.is_relative_to(ROOT)
        and not path.is_relative_to(ROOT / "third_party")
        and path.suffix in CPP_SUFFIXES
    )


def _build_files(gn: Path, variant: str) -> list[Path]:
    result = subprocess.run(
        [str(gn), "desc", str(output_directory(variant)), "//*", "sources", "--format=json"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    try:
        targets = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError("GN produced an invalid source listing") from error
    files: set[Path] = set()
    for target in targets.values():
        for source in target.get("sources", []):
            path = (ROOT / source.removeprefix("//")).resolve()
            if _is_project_file(path):
                files.add(path)
    return sorted(files)


def _path_files(paths: list[Path], build_files: list[Path]) -> list[Path]:
    files: set[Path] = set()
    for path in paths:
        candidate = (path if path.is_absolute() else Path.cwd() / path).resolve()
        if not candidate.exists():
            raise RuntimeError(f"lint path does not exist: {candidate}")
        if not candidate.is_relative_to(ROOT) or candidate.is_relative_to(ROOT / "third_party"):
            raise RuntimeError(f"lint only checks project-owned C++ files: {candidate}")
        if candidate.is_file():
            if candidate.suffix not in CPP_SUFFIXES:
                raise RuntimeError(f"lint path is not a C++ source or header: {candidate}")
            files.add(candidate)
            continue
        files.update(source for source in build_files if source.is_relative_to(candidate))
    return sorted(files)


def run(parsed: argparse.Namespace) -> int:
    generate_compilation_database(parsed.variant)
    tools = toolchain_tools()
    build_files = _build_files(tools["gn"], parsed.variant)
    files = _path_files(parsed.paths, build_files) if parsed.paths else build_files
    if not files:
        raise RuntimeError("no C++ source files or headers were selected for lint")

    failed = 0
    for source in files:
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
    print(f"linted {len(files)} C++ file(s), including headers; failures: {failed}")
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
