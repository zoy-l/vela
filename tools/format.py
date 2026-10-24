#!/usr/bin/env python3
"""Format Vela C++ and GN sources with the project tools."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from tools.config import ROOT, toolchain_tools


def _clang_format(tools: dict[str, Path]) -> Path:
    return tools["clang_format"]


def _project_files(values: list[str]) -> list[Path]:
    roots = [ROOT / value for value in values] if values else [ROOT]
    files: set[Path] = set()
    for root in roots:
        if not root.exists():
            raise RuntimeError(f"Formatting path does not exist: {root}")
        if root.is_file():
            candidates = [root]
        else:
            candidates = root.rglob("*")
        files.update(
            path
            for path in candidates
            if path.is_file()
            and path.suffix in {".cc", ".h", ".gn", ".gni"}
            and "out" not in path.parts
            and not (
                ROOT / "third_party" in path.resolve().parents
                and path.parent != ROOT / "third_party"
            )
        )
    return sorted(files)


def main(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", help="Files or directories under the project root")
    parsed = parser.parse_args(arguments)
    tools = toolchain_tools()
    clang_format = _clang_format(tools)
    for path in _project_files(parsed.paths):
        if path.suffix in {".gn", ".gni"}:
            subprocess.run([str(tools["gn"]), "format", str(path)], cwd=ROOT, check=True)
        else:
            subprocess.run([str(clang_format), "-i", str(path)], cwd=ROOT, check=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"format failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
