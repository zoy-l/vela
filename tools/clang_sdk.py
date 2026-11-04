"""Acquire the pinned Clang LibTooling SDK for the binding generator."""

from __future__ import annotations

import hashlib
from pathlib import Path, PurePosixPath
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request

from tools.config import BUILD_TOOLS_ROOT, host_platform
from tools.llvm import extract_sdk, sdk_specification, sync_license


def sdk_directory() -> Path:
    return BUILD_TOOLS_ROOT / "clang_sdk" / sdk_specification()[0] / host_platform()


def selected_member(path: PurePosixPath) -> bool:
    parts = path.parts
    if len(parts) < 2:
        return False
    if parts[0] == "include":
        return parts[1] in {"clang", "clang-c"}
    if parts[0] == "lib":
        return (parts[1] == "clang" or
                (path.name.startswith(("libclang", "clang")) and
                 path.suffix in {".a", ".lib"}))
    return parts[0] == "share" and "license" in str(path).lower()


def link_configuration() -> dict[str, str | list[str]]:
    directory = sdk_directory()
    if not (directory / "include/clang/Tooling/Tooling.h").is_file():
        raise RuntimeError(f"Missing Clang SDK: {directory}; run python -m tools.clang_sdk")
    # LLVM 21.1.8 的 clang/lib 各 CMakeLists.txt 定义此静态依赖闭包。
    components = ("Tooling", "Frontend", "Serialization", "Driver", "Parse", "Sema",
                  "Analysis", "ASTMatchers", "Format", "ToolingInclusions", "ToolingCore",
                  "Rewrite", "Edit", "APINotes", "AST", "Lex", "Basic", "Support")
    libraries = [directory / "lib" / (f"clang{name}.lib" if sys.platform == "win32"
                                      else f"libclang{name}.a") for name in components]
    for library in libraries:
        if not library.is_file():
            raise RuntimeError(f"Missing Clang static library: {library}")
    return {"vela_clang_root": directory.as_posix(),
            "vela_clang_libraries": [path.as_posix() for path in libraries]}


def sync() -> None:
    version, archive_name, expected, license_identity = sdk_specification()
    destination = sdk_directory()
    identity = destination / ".archive-sha256"
    if identity.is_file() and identity.read_text(encoding="ascii").strip() == expected:
        sync_license(destination, version, license_identity)
        print(f"Clang LibTooling SDK {version}: {destination}")
        return
    if destination.exists():
        raise RuntimeError(f"Incomplete or mismatched Clang SDK at {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    url = (f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{version}/"
           + urllib.parse.quote(archive_name))
    with tempfile.TemporaryDirectory(prefix="vela-clang-download-") as download:
        archive = Path(download) / archive_name
        print(f"Downloading Clang SDK {version} for {host_platform()}", flush=True)
        digest = hashlib.sha256()
        with urllib.request.urlopen(url, timeout=60) as response, archive.open("wb") as output:
            while chunk := response.read(1024 * 1024):
                digest.update(chunk)
                output.write(chunk)
        if digest.hexdigest() != expected:
            raise RuntimeError("Clang SDK SHA-256 does not match DEPS")
        print("Verified Clang archive; extracting AST/Tooling SDK", flush=True)
        with tempfile.TemporaryDirectory(prefix=".extract-", dir=destination.parent) as work:
            staged = Path(work) / "sdk"
            staged.mkdir()
            extract_sdk(archive, staged, selected_member)
            if not (staged / "include/clang/Tooling/Tooling.h").is_file():
                raise RuntimeError("Clang package lacks LibTooling development headers")
            sync_license(staged, version, license_identity)
            (staged / ".archive-sha256").write_text(expected + "\n", encoding="ascii")
            staged.rename(destination)
    print(f"Clang SDK ready: {destination}")


if __name__ == "__main__":
    try:
        sync()
    except (OSError, RuntimeError, tarfile.TarError) as error:
        print(f"Clang SDK acquisition failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
