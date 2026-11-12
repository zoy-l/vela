"""Acquire the pinned upstream LLVM tools, runtime resources, and development SDK."""

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

from tools.config import BUILD_TOOLS_ROOT, ROOT, executable_name, host_platform, llvm_tools


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
    return BUILD_TOOLS_ROOT / "llvm_toolchain" / version / host_platform()


def extract_sdk(archive: Path, destination: Path) -> None:
    destination = destination.resolve()
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
            if not relative.parts:
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
        # 发布包的工具、共享库和目录别名可能形成前向引用；只解析包内目标。
        while links:
            pending = []
            for target, member in links:
                if member.issym():
                    source = (target.parent / member.linkname).resolve()
                else:
                    original = PurePosixPath(member.linkname)
                    if not original.parts or original.is_absolute() or original.parts[0] != root_name:
                        raise RuntimeError(f"Invalid LLVM tool alias: {member.name}")
                    source = destination.joinpath(*original.parts[1:]).resolve()
                if not source.is_relative_to(destination):
                    raise RuntimeError(f"Invalid LLVM tool alias: {member.name}")
                if not source.exists():
                    pending.append((target, member))
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                if source.is_dir():
                    os.symlink(os.path.relpath(source, target.parent), target,
                               target_is_directory=True)
                else:
                    os.link(source, target)
            if len(pending) == len(links):
                raise RuntimeError(f"Unresolved LLVM tool alias: {pending[0][1].name}")
            links = pending


def validate_sdk(directory: Path, version: str) -> None:
    llvm_tools(directory)
    for relative in ("bin/" + executable_name("llvm-config"), "include/llvm/IR/Module.h",
                     "include/clang/Tooling/Tooling.h",
                     f"lib/clang/{version.split('.')[0]}/include/stddef.h"):
        if not (directory / relative).is_file():
            raise RuntimeError(f"LLVM toolchain lacks required input: {directory / relative}")


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
        validate_sdk(destination, version)
        sync_license(destination, version, license_identity)
        print(f"LLVM toolchain {version}: {destination}")
        return
    if destination.exists():
        raise RuntimeError(f"Incomplete or mismatched LLVM toolchain at {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    url = (f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{version}/"
           + urllib.parse.quote(archive_name))
    with tempfile.TemporaryDirectory(prefix="vela-llvm-download-") as download:
        archive = Path(download) / archive_name
        print(f"Downloading LLVM toolchain {version} for {host_platform()}", flush=True)
        digest = hashlib.sha256()
        with urllib.request.urlopen(url, timeout=60) as response, archive.open("wb") as output:
            while chunk := response.read(1024 * 1024):
                digest.update(chunk)
                output.write(chunk)
        if digest.hexdigest() != expected:
            raise RuntimeError("LLVM toolchain SHA-256 does not match DEPS")
        print("Verified LLVM archive; extracting complete toolchain and development SDK", flush=True)
        with tempfile.TemporaryDirectory(prefix=".extract-", dir=destination.parent) as work:
            staged = Path(work) / "sdk"
            staged.mkdir()
            extract_sdk(archive, staged)
            validate_sdk(staged, version)
            sync_license(staged, version, license_identity)
            (staged / ".archive-sha256").write_text(expected + "\n", encoding="ascii")
            staged.rename(destination)
    print(f"LLVM toolchain ready: {destination}")


def link_configuration() -> dict[str, str | list[str]]:
    directory = sdk_directory()
    tool = directory / "bin" / executable_name("llvm-config")
    if not tool.is_file():
        raise RuntimeError(f"Missing LLVM SDK: {directory}; run python -m tools.vl llvm sync")

    def query(*arguments: str) -> list[str]:
        result = subprocess.run([str(tool), "--link-static", *arguments], check=True,
                                capture_output=True, text=True, encoding="utf-8")
        return shlex.split(result.stdout, posix=True)

    def select(components: tuple[str, ...]) -> tuple[list[str], list[str]]:
        libraries = [directory / "lib" / name
                     for name in query("--libnames", *components)]
        for library in libraries:
            if not library.is_file():
                raise RuntimeError(f"Missing LLVM static library: {library}")
        system = []
        for argument in query("--system-libs", *components):
            name = Path(argument).name
            if name == "libxml2s.lib" and not any(
                    library.stem == "LLVMWindowsManifest" for library in libraries):
                # llvm-config 汇总了全部组件的系统库；libxml2 只属于 WindowsManifest，
                # 当前代码生成组件闭包不包含它。此处按组件归属筛选，不按文件缺失降级。
                continue
            if name in {"libzstd.a", "zstd.lib", "-lzstd"}:
                # 发布方的绝对安装路径不是本地依赖；zstd 由本仓库的固定源码构建。
                continue
            if argument.startswith("-l"):
                system.append(argument[2:])
            elif not Path(argument).is_absolute() and argument.endswith(".lib"):
                system.append(argument)
            else:
                raise RuntimeError(f"Unmanaged LLVM system library: {argument}")
        return [path.as_posix() for path in libraries], system

    core = ("core", "passes", "nativecodegen")
    libraries, system = select(core)
    jit_libraries, jit_system = select(core + ("orcjit", "orcdebugging", "jitlink",
                                             "orctargetprocess"))
    tooling_libraries, tooling_system = select((
        "bitreader", "bitstreamreader", "option", "plugins", "profiledata", "targetparser",
        "binaryformat", "mc", "mcparser", "object", "windowsdriver", "frontendhlsl",
        "frontendopenmp", "core", "demangle", "support"))
    return {"vela_llvm_root": directory.as_posix(),
            "vela_llvm_rtti": query("--has-rtti")[0],
            "vela_llvm_target": query("--host-target")[0],
            "vela_llvm_libraries": libraries,
            "vela_llvm_jit_libraries": jit_libraries,
            "vela_llvm_tooling_libraries": tooling_libraries,
            "vela_llvm_system_libraries": system,
            "vela_llvm_jit_system_libraries": jit_system,
            "vela_llvm_tooling_system_libraries": tooling_system}


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
        print(f"LLVM toolchain acquisition failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
