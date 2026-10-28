#!/usr/bin/env python3
"""Generate a Vela GN output directory from the project toolchain."""

from __future__ import annotations

import argparse
import hashlib
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


def static_library_name(name: str) -> str:
    """Return the archive name emitted by the repository's GN toolchain."""
    return name + (".lib" if platform.system() == "Windows" else ".a")


def _xcode_path(arguments: list[str], description: str) -> Path:
    result = subprocess.run(
        ["/usr/bin/xcrun", "--sdk", "macosx", *arguments],
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


def _xcode_linker_version(linker: Path) -> str:
    result = subprocess.run(
        [str(linker), "-version_details"],
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    try:
        version = json.loads(result.stdout)["version"]
    except (json.JSONDecodeError, KeyError, TypeError) as error:
        raise RuntimeError("Xcode linker did not report its version") from error
    if not isinstance(version, str) or not all(part.isdecimal() for part in version.split(".")):
        raise RuntimeError(f"Invalid Xcode linker version: {version!r}")
    return version


def toolchain_tools() -> dict[str, Path]:
    host = host_platform()
    clang_bin = BUILD_TOOLS_ROOT / "clang" / host / "bin"
    gn_path = BUILD_TOOLS_ROOT / "gn" / host / executable_name("gn")
    ninja_path = BUILD_TOOLS_ROOT / "ninja" / host / executable_name("ninja")
    if platform.system() == "Windows":
        cc = clang_bin / "clang-cl.exe"
        cxx = cc
        archiver = clang_bin / "llvm-lib.exe"
        linker = clang_bin / "lld-link.exe"
    else:
        cc = clang_bin / "clang"
        cxx = clang_bin / "clang++"
        archiver = clang_bin / "llvm-ar"
        linker = (
            _xcode_path(["--find", "ld"], "linker")
            if platform.system() == "Darwin"
            else clang_bin / "ld.lld"
        )
    tools = {
        "gn": _require_file(gn_path, "project GN executable"),
        "ninja": _require_file(ninja_path, "project Ninja executable"),
        "cc": _require_file(cc, "project Clang executable"),
        "cxx": _require_file(cxx, "project Clang++ executable"),
        "archiver": _require_file(archiver, "project LLVM archiver"),
        "linker": _require_file(linker, "host linker"),
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
    if platform.system() == "Darwin":
        tools["sysroot"] = _xcode_path(["--show-sdk-path"], "macOS SDK")
    return tools


def _gn_value(value: str | list[str]) -> str:
    normalized = ([item.replace("\\", "/") for item in value] if isinstance(value, list)
                  else value.replace("\\", "/"))
    return json.dumps(normalized)


def compiler_build_identity() -> str:
    """Invalidate compiled caches when compiler, runtime, ABI or pinned tools change."""
    paths = {ROOT / "DEPS", ROOT / "BUILD.gn", ROOT / "tools" / "config.py",
             ROOT / "third_party" / "BUILD.gn"}
    paths.update({ROOT / "third_party/unicode_data/manifest.json", ROOT / "tools/unicode_case.py"})
    paths.update((ROOT / "third_party").glob("*.gni"))
    for directory in ("base", "compiler", "runtime", "vm", "build"):
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


def configure(variant: str) -> Path:
    from tools.llvm import link_configuration

    tools = toolchain_tools()
    output = output_directory(variant)
    args = {
        "target_cpu": target_cpu(),
        "target_os": target_os(),
        "vela_archiver": tools["archiver"].as_posix(),
        "vela_build_config": variant,
        "vela_build_identity": compiler_build_identity(),
        "vela_cc": tools["cc"].as_posix(),
        "vela_cxx": tools["cxx"].as_posix(),
        "vela_linker": tools["linker"].as_posix(),
        "vela_python": Path(sys.executable).resolve().as_posix(),
    }
    args.update(link_configuration())
    if platform.system() == "Darwin":
        args["vela_sysroot"] = tools["sysroot"].as_posix()
        args["vela_linker_version"] = _xcode_linker_version(tools["linker"])
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
