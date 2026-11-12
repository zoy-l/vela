#!/usr/bin/env python3
"""Generate a Vela GN output directory from the project toolchain."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
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


def require_file(path: Path, description: str) -> Path:
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


def static_library_name(name: str) -> str:
    """Return the archive name emitted by the repository's GN toolchain."""
    return name + (".lib" if platform.system() == "Windows" else ".a")


def xcode_path(arguments: list[str], description: str) -> Path:
    sdk = os.environ.get("SDKROOT") or "macosx"
    result = subprocess.run(
        ["/usr/bin/xcrun", "--sdk", sdk, *arguments],
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    value = result.stdout.strip()
    path = Path(value)
    if not value or not path.exists():
        raise RuntimeError(f"Missing Xcode {description}: {value}; install Xcode")
    return path.resolve()


def llvm_tools(directory: Path) -> dict[str, Path]:
    clang_bin = directory / "bin"
    system = platform.system()
    if system == "Windows":
        cc = clang_bin / "clang-cl.exe"
        cxx = cc
        archiver = clang_bin / "llvm-lib.exe"
        linker = clang_bin / "lld-link.exe"
    else:
        cc = clang_bin / "clang"
        cxx = clang_bin / "clang++"
        archiver = clang_bin / "llvm-ar"
        linker = clang_bin / ("ld64.lld" if system == "Darwin" else "ld.lld")
    return {
        "cc": require_file(cc, "project Clang executable"),
        "cxx": require_file(cxx, "project Clang++ executable"),
        "archiver": require_file(archiver, "project LLVM archiver"),
        "linker": require_file(linker, "project LLD linker"),
        "clang_format": require_file(
            clang_bin / executable_name("clang-format"),
            "project clang-format executable",
        ),
        "clangd": require_file(
            clang_bin / executable_name("clangd"),
            "project clangd executable",
        ),
        "clang_tidy": require_file(
            clang_bin / executable_name("clang-tidy"),
            "project clang-tidy executable",
        ),
    }


def toolchain_tools() -> dict[str, Path]:
    from tools.llvm import sdk_directory

    host = host_platform()
    tools = {
        "gn": require_file(BUILD_TOOLS_ROOT / "gn" / host / executable_name("gn"),
                           "project GN executable"),
        "ninja": require_file(BUILD_TOOLS_ROOT / "ninja" / host / executable_name("ninja"),
                              "project Ninja executable"),
        **llvm_tools(sdk_directory()),
    }
    if platform.system() == "Darwin":
        tools["sysroot"] = xcode_path(["--show-sdk-path"], "macOS SDK")
    return tools


def gn_value(value: str | bool | list[str]) -> str:
    if isinstance(value, bool):
        return json.dumps(value)
    normalized = ([item.replace("\\", "/") for item in value] if isinstance(value, list)
                  else value.replace("\\", "/"))
    return json.dumps(normalized)


def compiler_build_identity() -> str:
    """Invalidate compiled caches when compiler, runtime, ABI or pinned tools change."""
    paths = {ROOT / "DEPS", ROOT / "BUILD.gn", ROOT / "tools" / "config.py",
             ROOT / "third_party" / "BUILD.gn"}
    paths.update({ROOT / "third_party/unicode_data/manifest.json", ROOT / "tools/unicode_case.py"})
    paths.update((ROOT / "third_party").glob("*.gni"))
    for directory in ("base", "compiler", "runtime", "include", "build"):
        paths.update(path for path in (ROOT / directory).rglob("*")
                     if path.suffix in {".h", ".cc", ".gn", ".gni"}
                     and not path.name.endswith("_unittest.cc"))
    digest = hashlib.sha256()
    digest.update(f"{target_os()}/{target_cpu()}".encode())
    for path in sorted(paths):
        name = path.relative_to(ROOT).as_posix().encode()
        content = path.read_bytes()
        digest.update(len(name).to_bytes(8, "little"))
        digest.update(name)
        digest.update(len(content).to_bytes(8, "little"))
        digest.update(content)
    return digest.hexdigest()


def configure(variant: str, *, cc_wrapper: str = "sccache") -> Path:
    from tools.clang_sdk import link_configuration as clang_link_configuration
    from tools.llvm import link_configuration

    tools = toolchain_tools()
    if cc_wrapper:
        wrapper = shutil.which(cc_wrapper)
        if wrapper is None:
            raise RuntimeError(
                f"Missing compiler wrapper: {cc_wrapper}; install it or pass --cc-wrapper=''"
            )
        cc_wrapper = Path(wrapper).resolve().as_posix()
    output = output_directory(variant)
    args = {
        "cc_wrapper": cc_wrapper,
        "target_cpu": target_cpu(),
        "target_os": target_os(),
        "vela_archiver": tools["archiver"].as_posix(),
        "vela_build_config": variant,
        "vela_thin_lto": variant == "release",
        "vela_build_identity": compiler_build_identity(),
        "vela_cc": tools["cc"].as_posix(),
        "vela_cxx": tools["cxx"].as_posix(),
        "vela_linker": tools["linker"].as_posix(),
        "vela_python": Path(sys.executable).resolve().as_posix(),
    }
    args.update(link_configuration())
    args.update(clang_link_configuration())
    if platform.system() == "Darwin":
        args["vela_sysroot"] = tools["sysroot"].as_posix()
    encoded_args = " ".join(
        f"{key}={gn_value(value)}" for key, value in args.items()
    )
    command = [
        str(tools["gn"]),
        "gen",
        str(output),
        f"--args={encoded_args}",
        "--root-target=//:default",
        "--check=system",
        "--export-compile-commands",
    ]
    subprocess.run(command, cwd=ROOT, check=True)
    return output


def add_compiler_wrapper_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--cc-wrapper", default="sccache",
        help="Compiler wrapper executable (default: sccache; pass an empty string to disable)",
    )


def parse_arguments(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("variant", choices=BUILD_VARIANTS, default="debug", nargs="?")
    add_compiler_wrapper_argument(parser)
    return parser.parse_args(arguments)


def main(arguments: list[str] | None = None) -> int:
    parsed = parse_arguments(arguments)
    configure(parsed.variant, cc_wrapper=parsed.cc_wrapper)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"config failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
