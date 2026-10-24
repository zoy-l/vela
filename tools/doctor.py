#!/usr/bin/env python3
"""Check that the project-managed toolchain is available."""

from __future__ import annotations

from tools.config import BUILD_TOOLS_ROOT, host_platform, toolchain_tools


def main() -> int:
    tools = toolchain_tools()
    print(f"toolchain root: {BUILD_TOOLS_ROOT}")
    print(f"host platform: {host_platform()}")
    for name in ("gn", "ninja", "cc", "cxx", "archiver", "clang_format", "clangd"):
        print(f"{name}: {tools[name]}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"doctor failed: {error}")
        raise SystemExit(1) from error
