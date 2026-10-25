#!/usr/bin/env python3
"""Dispatch Vela developer commands."""

from __future__ import annotations

import subprocess
import sys


COMMAND_MODULES = {
    "build": "tools.build",
    "bench": "tools.bench",
    "clangd": "tools.clangd",
    "config": "tools.config",
    "doctor": "tools.doctor",
    "editor": "tools.editor",
    "format": "tools.format",
    "lint": "tools.lint",
    "test": "tools.test",
}


def print_help() -> None:
    print("Usage: vl <command> [arguments]")
    print()
    print("Commands:")
    print("  build       Configure and build a target")
    print("  bench       Build Release and run benchmark workloads")
    print("  clangd      Generate compile_commands.json")
    print("  config      Generate a GN output directory")
    print("  doctor      Check the project toolchain")
    print("  editor      Package the VS Code extension")
    print("  format      Format C++ and GN sources")
    print("  lint        Run project clang-tidy checks")
    print("  test        Run GoogleTest suites")


def main() -> int:
    if len(sys.argv) == 1 or sys.argv[1] in {"help", "--help", "-h"}:
        print_help()
        return 0 if len(sys.argv) > 1 else 2
    module = COMMAND_MODULES.get(sys.argv[1])
    if module is None:
        print(f"Unknown vl command: {sys.argv[1]}", file=sys.stderr)
        print_help()
        return 2
    return subprocess.run(
        [sys.executable, "-m", module, *sys.argv[2:]], check=False
    ).returncode


if __name__ == "__main__":
    raise SystemExit(main())
