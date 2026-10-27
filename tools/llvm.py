"""Acquire the pinned LLVM development SDK used by the native compiler backend."""

from __future__ import annotations

import argparse
import ast
import hashlib
import os
from pathlib import Path, PurePosixPath
import shutil
import shlex
import subprocess
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request

from tools.config import BUILD_TOOLS_ROOT, ROOT, executable_name, host_platform


def sdk_specification() -> tuple[str, str, str, str]:
    tree = ast.parse((ROOT / "DEPS").read_text(encoding="utf-8"))
    declaration = next(
        node for node in tree.body
        if isinstance(node, ast.Assign)
        and any(isinstance(target, ast.Name) and target.id == "vars"
                for target in node.targets)
    )
    variables = ast.literal_eval(declaration.value)
    prefix = "llvm_sdk_" + host_platform().replace("-", "_")
    return (variables["llvm_sdk_version"], variables[prefix + "_archive"],
            variables[prefix + "_sha256"], variables["llvm_sdk_license_sha256"])


def sdk_directory() -> Path:
    version, _, _, _ = sdk_specification()
    return BUILD_TOOLS_ROOT / "llvm" / version / host_platform()


def selected_member(path: PurePosixPath) -> bool:
    parts = path.parts
    if len(parts) < 2:
        return False
    if parts[0] == "include":
        return parts[1] in {"llvm", "llvm-c"}
    if parts[0] == "lib":
        return (path.name.startswith(("libLLVM", "LLVM"))
                and path.suffix in {".a", ".lib"})
    if parts[0] == "bin":
        return path.name.removesuffix(".exe") in {
            "llvm-config", "llvm-ar", "llvm-lib", "llvm-ranlib", "llvm-libtool-darwin",
            "lld", "ld.lld", "ld64.lld", "lld-link", "wasm-ld",
        }
    return parts[0] == "share" and "license" in str(path).lower()


def extract_sdk(archive: Path, destination: Path) -> None:
    with tarfile.open(archive, mode="r|xz") as package:
        root_name = None
        links = []
        for member in package:
            original = PurePosixPath(member.name)
            if original.is_absolute() or ".." in original.parts:
                raise RuntimeError(f"Invalid LLVM archive path: {original}")
            if not original.parts:
                continue
            if root_name is None:
                root_name = original.parts[0]
            if original.parts[0] != root_name:
                raise RuntimeError("LLVM archive contains multiple roots")
            relative = PurePosixPath(*original.parts[1:])
            if not selected_member(relative):
                continue
            target = destination.joinpath(*relative.parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                with package.extractfile(member) as source, target.open("wb") as output:
                    shutil.copyfileobj(source, output)
                target.chmod(member.mode & 0o777)
            elif member.issym() or member.islnk():
                links.append((target, member))
            else:
                raise RuntimeError(f"Unsupported LLVM archive entry: {original}")
        # 工具别名只连接包内已选中的文件，不发布指向 SDK 外部的符号链接。
        for target, member in links:
            source = ((target.parent / member.linkname).resolve() if member.issym()
                      else destination.joinpath(*PurePosixPath(member.linkname).parts[1:]))
            if not source.is_relative_to(destination.resolve()) or not source.is_file():
                raise RuntimeError(f"Invalid LLVM tool alias: {member.name}")
            target.parent.mkdir(parents=True, exist_ok=True)
            os.link(source, target)


def sync_license(destination: Path, version: str, expected: str) -> None:
    license_file = destination / "LICENSE.TXT"
    if license_file.is_file() and hashlib.sha256(license_file.read_bytes()).hexdigest() == expected:
        return
    url = f"https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-{version}/llvm/LICENSE.TXT"
    with urllib.request.urlopen(url, timeout=60) as response:
        data = response.read()
    if hashlib.sha256(data).hexdigest() != expected:
        raise RuntimeError("LLVM license SHA-256 does not match DEPS")
    license_file.write_bytes(data)


def sync() -> None:
    version, archive_name, expected, license_identity = sdk_specification()
    destination = sdk_directory()
    identity = destination / ".archive-sha256"
    if identity.is_file() and identity.read_text(encoding="ascii").strip() == expected:
        sync_license(destination, version, license_identity)
        print(f"LLVM SDK {version}: {destination}")
        return
    if destination.exists():
        raise RuntimeError(f"Incomplete or mismatched LLVM SDK at {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    url = (f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{version}/"
           + urllib.parse.quote(archive_name))
    with tempfile.TemporaryDirectory(prefix="vela-llvm-download-") as download:
        archive = Path(download) / archive_name
        print(f"Downloading LLVM SDK {version} for {host_platform()}", flush=True)
        digest = hashlib.sha256()
        with urllib.request.urlopen(url, timeout=60) as response, archive.open("wb") as output:
            while chunk := response.read(1024 * 1024):
                digest.update(chunk)
                output.write(chunk)
        if digest.hexdigest() != expected:
            raise RuntimeError("LLVM SDK SHA-256 does not match DEPS")
        print("Verified LLVM archive; extracting headers, static libraries and link tools",
              flush=True)
        with tempfile.TemporaryDirectory(prefix=".extract-", dir=destination.parent) as work:
            staged = Path(work) / "sdk"
            staged.mkdir()
            extract_sdk(archive, staged)
            if not (staged / "include/llvm/IR/Module.h").is_file():
                raise RuntimeError("LLVM package lacks development headers")
            sync_license(staged, version, license_identity)
            (staged / ".archive-sha256").write_text(expected + "\n", encoding="ascii")
            staged.rename(destination)
    print(f"LLVM SDK ready: {destination}")


def link_configuration() -> dict[str, str | list[str]]:
    directory = sdk_directory()
    tool = directory / "bin" / executable_name("llvm-config")
    if not tool.is_file():
        raise RuntimeError(f"Missing LLVM SDK: {directory}; run vl llvm sync")

    def query(*arguments: str) -> list[str]:
        result = subprocess.run([str(tool), "--link-static", *arguments], check=True,
                                capture_output=True, text=True, encoding="utf-8")
        return shlex.split(result.stdout, posix=True)

    libraries = [directory / "lib" / name
                 for name in query("--libnames", "core", "passes", "nativecodegen")]
    for library in libraries:
        if not library.is_file():
            raise RuntimeError(f"Missing LLVM static library: {library}")
    system = []
    for argument in query("--system-libs"):
        name = Path(argument).name
        if name in {"libzstd.a", "zstd.lib", "-lzstd"}:
            # 发布方的绝对安装路径不是本地依赖；zstd 由本仓库的固定源码构建。
            continue
        if argument.startswith("-l"):
            system.append(argument[2:])
        elif not Path(argument).is_absolute() and argument.endswith(".lib"):
            system.append(argument)
        else:
            raise RuntimeError(f"Unmanaged LLVM system library: {argument}")
    return {"vela_llvm_root": directory.as_posix(),
            "vela_llvm_libraries": [path.as_posix() for path in libraries],
            "vela_llvm_system_libraries": system}


def main(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("sync",))
    parser.parse_args(arguments)
    sync()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, tarfile.TarError) as error:
        print(f"LLVM SDK acquisition failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
