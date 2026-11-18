#!/usr/bin/env python3
"""Build steps used by the Vela GN toolchain."""

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

    link_and_strip = subparsers.add_parser("link-and-strip")
    link_and_strip.add_argument("--strip-tool", required=True)
    link_and_strip.add_argument("--output", required=True)
    link_and_strip.add_argument("driver")
    link_and_strip.add_argument("link_args", nargs=argparse.REMAINDER)

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
    elif arguments.command == "link-and-strip":
        result = subprocess.run([arguments.driver, *arguments.link_args], check=False)
        if result.returncode != 0:
            return result.returncode
        return subprocess.run(
            [arguments.strip_tool, "--strip-all", arguments.output], check=False
        ).returncode
    elif arguments.command == "copy":
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(arguments.source, arguments.output)
    else:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.touch()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
