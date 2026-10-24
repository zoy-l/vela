#!/usr/bin/env python3
"""File operations used by the Vela GN toolchain."""

from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    archive = subparsers.add_parser("archive")
    archive.add_argument("--tool", required=True, type=Path)
    archive.add_argument("--output", required=True, type=Path)
    archive.add_argument("--rspfile", required=True, type=Path)

    copy = subparsers.add_parser("copy")
    copy.add_argument("source", type=Path)
    copy.add_argument("output", type=Path)

    touch = subparsers.add_parser("touch")
    touch.add_argument("output", type=Path)

    arguments = parser.parse_args()
    if arguments.command == "archive":
        arguments.output.unlink(missing_ok=True)
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        if arguments.tool.stem.lower() == "llvm-lib":
            command = [
                str(arguments.tool),
                f"/OUT:{arguments.output}",
                f"@{arguments.rspfile}",
            ]
        else:
            command = [
                str(arguments.tool),
                "rcs",
                str(arguments.output),
                f"@{arguments.rspfile}",
            ]
        subprocess.run(command, check=True)
    elif arguments.command == "copy":
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(arguments.source, arguments.output)
    else:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.touch()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
