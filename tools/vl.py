#!/usr/bin/env python3
"""Dispatch Vela developer commands."""

from __future__ import annotations

import subprocess
import sys


COMMANDS = {
    "build": "Configure and build a target",
    "bench": "Run Release benchmarks, comparisons and --gc-report",
    "bindgen": "Generate or verify native class bindings from C++ headers",
    "clangd": "Generate compile_commands.json",
    "config": "Generate a GN output directory",
    "doctor": "Check the project toolchain",
    "editor": "Package the VS Code extension",
    "embed": "Export a relocatable embedding SDK",
    "format": "Format C++ and GN sources",
    "lint": "Run project clang-tidy checks",
    "llvm": "Acquire the pinned LLVM toolchain and development SDK with llvm sync",
    "package": "Build a distribution with audited native target support",
    "test": "Run GoogleTest suites and tooling tests",
}


def print_help() -> None:
    print("Usage: vl <command> [arguments]")
    print()
    print("Commands:")
    for name, description in COMMANDS.items():
        print(f"  {name:<12}{description}")


def main() -> int:
    if len(sys.argv) == 1 or sys.argv[1] in {"help", "--help", "-h"}:
        print_help()
        return 0 if len(sys.argv) > 1 else 2
    command = sys.argv[1]
    if command not in COMMANDS:
        print(f"Unknown vl command: {command}", file=sys.stderr)
        print_help()
        return 2
    return subprocess.run(
        [sys.executable, "-m", f"tools.{command}", *sys.argv[2:]], check=False
    ).returncode


if __name__ == "__main__":
    raise SystemExit(main())
