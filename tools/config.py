#!/usr/bin/env python3
"""Generate a Vela GN output directory from the project toolchain."""

from __future__ import annotations

import argparse
import json
import platform
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD_VARIANTS = ("debug", "release")
BUILD_TOOLS_ROOT = ROOT / "third_party" / "build_tools"


def output_directory(variant: str) -> Path:
    if variant not in BUILD_VARIANTS:
        raise ValueError(f"Unsupported build variant: {variant}")
    return ROOT / "out" / variant


def _require_file(path: Path, description: str) -> Path:
    if not path.is_file():
        raise RuntimeError(f"Missing {description}: {path}; run gclient sync --no-history")
    return path


def host_platform() -> str:
    system = platform.system()
    machine = platform.machine().lower()
    if system == "Windows" and machine in {"amd64", "x86_64"}:
        return "windows-amd64"
    if system == "Linux" and machine in {"amd64", "x86_64"}:
        return "linux-amd64"
    if system == "Darwin" and machine in {"arm64", "aarch64"}:
        return "mac-arm64"
    raise RuntimeError(
        f"Unsupported development host: {system} {platform.machine()}; "
        "supported hosts are Windows x64, Linux x64, and macOS arm64"
    )


def target_os() -> str:
    return {"Windows": "win", "Linux": "linux", "Darwin": "mac"}[platform.system()]


def target_cpu() -> str:
    return "arm64" if host_platform() == "mac-arm64" else "x64"


def executable_name(name: str) -> str:
    return f"{name}.exe" if platform.system() == "Windows" else name


def toolchain_tools() -> dict[str, Path]:
    host = host_platform()
    clang_bin = BUILD_TOOLS_ROOT / "clang" / host / "bin"
    gn_path = BUILD_TOOLS_ROOT / "gn" / host / executable_name("gn")
    ninja_path = BUILD_TOOLS_ROOT / "ninja" / host / executable_name("ninja")
    if platform.system() == "Windows":
        cc = clang_bin / "clang++.exe"
        cxx = clang_bin / "clang++.exe"
        archiver = clang_bin / "llvm-lib.exe"
    else:
        cc = clang_bin / "clang"
        cxx = clang_bin / "clang++"
        archiver = clang_bin / "llvm-ar"
    return {
        "gn": _require_file(gn_path, "project GN executable"),
        "ninja": _require_file(ninja_path, "project Ninja executable"),
        "cc": _require_file(cc, "project Clang executable"),
        "cxx": _require_file(cxx, "project Clang++ executable"),
        "archiver": _require_file(archiver, "project LLVM archiver"),
        "clang_format": _require_file(
            clang_bin / executable_name("clang-format"),
            "project clang-format executable",
        ),
        "clangd": _require_file(
            clang_bin / executable_name("clangd"),
            "project clangd executable",
        ),
        "clang_tidy": _require_file(
            clang_bin / executable_name("clang-tidy"),
            "project clang-tidy executable",
        ),
    }


def _gn_value(value: str) -> str:
    return json.dumps(value.replace("\\", "/"))


def configure(variant: str) -> Path:
    tools = toolchain_tools()
    output = output_directory(variant)
    args = {
        "target_cpu": target_cpu(),
        "target_os": target_os(),
        "vela_archiver": tools["archiver"].as_posix(),
        "vela_build_config": variant,
        "vela_cc": tools["cc"].as_posix(),
        "vela_cxx": tools["cxx"].as_posix(),
        "vela_python": Path(sys.executable).resolve().as_posix(),
    }
    encoded_args = " ".join(
        f"{key}={_gn_value(value)}" for key, value in args.items()
    )
    command = [
        str(tools["gn"]),
        "gen",
        str(output),
        f"--args={encoded_args}",
        "--root-target=//:default",
        "--check=system",
    ]
    subprocess.run(command, cwd=ROOT, check=True)
    return output


def parse_arguments(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("variant", choices=BUILD_VARIANTS, default="debug", nargs="?")
    return parser.parse_args(arguments)


def main(arguments: list[str] | None = None) -> int:
    parsed = parse_arguments(arguments)
    configure(parsed.variant)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"config failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
