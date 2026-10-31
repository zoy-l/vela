"""Build a native Vela distribution with pinned archive/link tools and audited inputs."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

from tools.config import BUILD_VARIANTS, ROOT, configure, executable_name, static_library_name, toolchain_tools
from tools.llvm import sdk_directory, sdk_specification


def command(arguments: list[str | Path], *, environment: dict[str, str] | None = None,
            directory: Path | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(list(map(str, arguments)), check=True, capture_output=True,
                          text=True, encoding="utf-8", errors="replace",
                          env=environment, cwd=directory)


def digest(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def copy_file(source: Path, package: Path, relative: str, role: str, source_name: str,
              license_name: str) -> dict:
    path = Path(relative)
    if path.is_absolute() or ".." in path.parts or path == Path("."):
        raise RuntimeError(f"Packaged file requires a package-relative destination: {relative}")
    destination = package / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    return {"path": "{package}/" + relative, "sha256": digest(destination), "role": role,
            "source": source_name, "license": license_name, "redistribution": "bundled"}


def load_support_specification(path: Path, target: str) -> dict:
    support = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(support, dict) or support.get("target") != target:
        raise RuntimeError("Support specification target does not match the native compiler")
    for field in ("minimum_os", "object_format", "runtime_variant"):
        if not isinstance(support.get(field), str) or not support[field]:
            raise RuntimeError(f"Support specification requires {field}")
    for field in ("files", "external_roots", "application_dependencies", "archive_arguments",
                  "link_arguments", "bundled_files"):
        if field == "bundled_files":
            support.setdefault(field, [])
        if not isinstance(support.get(field), list):
            raise RuntimeError(f"Support specification requires {field}")
    for field in ("archive_arguments", "link_arguments", "application_dependencies"):
        if (field != "application_dependencies" and not support[field]) or any(
                not isinstance(value, str) for value in support[field]):
            raise RuntimeError(f"Support specification requires string arguments in {field}")
    contracts = {
        "files": ("path", "sha256", "role", "source", "license", "redistribution"),
        "external_roots": ("name", "environment", "version", "source", "license", "instructions"),
        "bundled_files": ("source_path", "destination", "source", "license"),
    }
    for field, required in contracts.items():
        for entry in support[field]:
            if not isinstance(entry, dict) or any(
                    not isinstance(entry.get(name), str) or not entry[name] for name in required):
                raise RuntimeError(f"Support specification {field} requires {', '.join(required)}")
    return support


def verify_package(package: Path, workspace: Path, sdk_environment: dict[str, str]) -> None:
    """Exercise the staged compiler, linker and runtime before publishing the package."""
    source = workspace / "package-check.vela"
    application = workspace / executable_name("package-check")
    marker = "Vela native package verified\n"
    source.write_text("async func main(): Future<void> { await Future.value(0); "
                      "print('Vela native package verified'); }\n", encoding="utf-8", newline="\n")
    excluded = {"PATH", "INCLUDE", "LIB", "LIBPATH", "VCTOOLSINSTALLDIR", "WINDOWSSDKDIR",
                "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH"}
    environment = {key: value for key, value in os.environ.items() if key.upper() not in excluded}
    environment.update(sdk_environment)
    environment["PATH"] = ""
    try:
        command([package / "bin" / executable_name("vela"), "build", source, "-o", application],
                environment=environment, directory=workspace)
        executed = command([application], environment=environment, directory=workspace)
    except subprocess.CalledProcessError as error:
        raise RuntimeError(f"Native package self-check failed (exit {error.returncode}): "
                           f"{error.stderr or error.stdout}") from error
    if executed.stdout != marker or executed.stderr:
        raise RuntimeError("Native package self-check produced unexpected output: "
                           + executed.stdout + executed.stderr)


def windows_support(runtime: Path) -> tuple[dict, dict[str, str]]:
    """Probe the development SDK once; final user builds invoke only the packaged linker."""
    tools = toolchain_tools()
    with tempfile.TemporaryDirectory(prefix="vela-sdk-probe-") as temporary:
        work = Path(temporary)
        source = work / "main.cc"
        source.write_text("int main() { return 0; }\n", encoding="utf-8", newline="\n")
        driver = command([tools["cxx"], "-###", "/MT", source])
        invocation = shlex.split(driver.stderr.splitlines()[-1])
        directories = [Path(value.split(":", 1)[1]) for value in invocation
                       if value.startswith("-libpath:")]
        msvc_lib = next(path for path in directories if (path / "libcmt.lib").is_file())
        ucrt_lib = next(path for path in directories if (path / "libucrt.lib").is_file())
        um_lib = next(path for path in directories if (path / "kernel32.lib").is_file())
        msvc = msvc_lib.parent.parent
        sdk = ucrt_lib.parents[3]
        version = ucrt_lib.parent.parent.name
        command([tools["cxx"], "/nologo", "/c", "/MT", source, "/Fo" + str(work / "main.obj")])
        linker = sdk_directory() / "bin/lld-link.exe"
        linked = command([linker, "/nologo", "/out:" + str(work / "main.exe"),
                          work / "main.obj", "/wholearchive:" + str(runtime), "/verbose",
                          *["/libpath:" + str(path) for path in (msvc_lib, ucrt_lib, um_lib)]])
        loaded = set()
        for line in linked.stderr.splitlines():
            prefix = "lld-link: Reading "
            if line.startswith(prefix):
                path = Path(line.removeprefix(prefix))
                if path.is_absolute() and path.suffix.lower() == ".lib":
                    loaded.add(path)
        if not loaded:
            raise RuntimeError("Pinned linker did not report SDK input files")
        inputs = []
        for path in sorted(loaded):
            if path.is_relative_to(msvc):
                name, root, license_name = "msvc", msvc, "Microsoft Visual Studio license"
            elif path.is_relative_to(sdk):
                name, root, license_name = "windows_sdk", sdk, "Microsoft Windows SDK license"
            else:
                raise RuntimeError(f"Undeclared SDK input: {path}")
            inputs.append({"path": "{" + name + "}/" + path.relative_to(root).as_posix(),
                           "sha256": digest(path), "role": "link", "source": name,
                           "license": license_name, "redistribution": "external SDK; not bundled"})
        dependencies = command([tools["cxx"].parent / "llvm-readobj.exe", "--coff-imports",
                                work / "main.exe"]).stdout
        external_roots = [
            {"name": "msvc", "environment": "VELA_MSVC_TOOLS", "version": msvc.name,
             "source": "https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022",
             "license": "Microsoft Visual Studio license; SDK link inputs are not redistributed",
             "instructions": f"Install MSVC {msvc.name} C++ Build Tools and set VELA_MSVC_TOOLS "
                             "to the versioned VC/Tools/MSVC directory; the Visual Studio IDE is not required"},
            {"name": "windows_sdk", "environment": "VELA_WINDOWS_SDK", "version": version,
             "source": "https://developer.microsoft.com/windows/downloads/windows-sdk/",
             "license": "Microsoft Windows SDK license; SDK link inputs are not redistributed",
             "instructions": f"Install Windows SDK/UCRT {version} and set VELA_WINDOWS_SDK "
                             "to its Windows Kits/10 root"},
        ]
        return ({
            "external_roots": external_roots, "files": inputs,
            "archive_arguments": ["/nologo", "/out:{output}", "{object}"],
            "link_arguments": ["/nologo", "/machine:x64", "/subsystem:console",
                               "/out:{output}", "{object}", "{runtime}",
                               "/libpath:{msvc}/lib/x64",
                               f"/libpath:{{windows_sdk}}/Lib/{version}/ucrt/x64",
                               f"/libpath:{{windows_sdk}}/Lib/{version}/um/x64"],
            "application_dependencies": sorted(set(re.findall(r"Name: (\S+)", dependencies))),
            "minimum_os": "Windows 10 (matching SDK and MSVC runtime requirements)",
            "object_format": "COFF", "runtime_variant": "static Release CRT; iterator debug level 0",
        }, {"VELA_MSVC_TOOLS": str(msvc), "VELA_WINDOWS_SDK": str(sdk)})


def create_package(executable: Path, runtime: Path, destination: Path,
                   support_spec: Path | None = None) -> dict[str, str]:
    """Publish a new directory only after every packaged file and target input is recorded."""
    executable, runtime, destination = (path.resolve() for path in (executable, runtime, destination))
    if destination.exists():
        raise RuntimeError(f"Package destination already exists: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".vela-package-", dir=destination.parent) as temporary:
        package = Path(temporary) / "package"
        package.mkdir()
        probe = Path(temporary) / "identity.vela"
        probe.write_text("export func identity(): int { return 0; }\n", encoding="utf-8", newline="\n")
        obj = Path(temporary) / "identity.o"
        command([executable, "build", probe, "--emit=obj", "--export=identity", "-o", obj])
        identity = json.loads(Path(str(obj) + ".aot.json").read_text(encoding="utf-8"))
        target = identity["target"]
        llvm_version = sdk_specification()[0]
        if support_spec is not None:
            support = load_support_specification(support_spec, target)
            for bundled in support.pop("bundled_files", []):
                support["files"].append(copy_file(
                    support_spec.parent / bundled["source_path"], package, bundled["destination"],
                    "link", bundled["source"], bundled["license"]))
            environment = {}
        elif sys.platform == "win32":
            support, environment = windows_support(runtime)
        else:
            raise RuntimeError("This host requires an audited --support-spec with its SDK/sysroot, "
                               "link inputs, licenses, minimum OS, and linker arguments")
        library_name = "vela_runtime.lib" if sys.platform == "win32" else "libvela_runtime.a"
        runtime_relative = f"lib/{target}/{library_name}"
        archive_name = "llvm-lib" if sys.platform == "win32" else "llvm-ar"
        link_name = "lld-link" if sys.platform == "win32" else "ld64.lld" if sys.platform == "darwin" else "ld.lld"
        archiver = "libexec/" + executable_name(archive_name)
        linker = "libexec/" + executable_name(link_name)
        files = support["files"]
        for source, relative, role, owner, license_name in (
                (executable, "bin/" + executable_name("vela"), "compiler", "Vela", "MIT"),
                (runtime, runtime_relative, "runtime", "Vela and third-party runtime", "share/vela/licenses"),
                (sdk_directory() / "bin" / executable_name(archive_name), archiver, "archive", f"LLVM {llvm_version}", "Apache-2.0 WITH LLVM-exception"),
                (sdk_directory() / "bin" / executable_name(link_name), linker, "link", f"LLVM {llvm_version}", "Apache-2.0 WITH LLVM-exception")):
            files.append(copy_file(source, package, relative, role, owner, license_name))
        # 嵌入宿主需要完整的公共头依赖；不打包编译器实现。
        for directory in ("base", "runtime"):
            for header in (ROOT / directory).rglob("*.h"):
                relative = (Path("include") / header.relative_to(ROOT)).as_posix()
                copy_file(header, package, relative, "header", "Vela", "MIT")
        for header in (ROOT / "third_party/abseil-cpp/absl").rglob("*"):
            if header.is_file() and header.suffix in {".h", ".inc"}:
                relative = header.relative_to(ROOT / "third_party/abseil-cpp").as_posix()
                copy_file(header, package, "include/" + relative, "header", "Abseil", "Apache-2.0")
        for name, path in {
            "vela": ROOT / "LICENSE", "llvm": sdk_directory() / "LICENSE.TXT",
            "abseil": ROOT / "third_party/abseil-cpp/LICENSE",
            "pcre2": ROOT / "third_party/pcre2/LICENCE.md",
            "utf8proc": ROOT / "third_party/utf8proc/LICENSE.md",
            "yyjson": ROOT / "third_party/yyjson/LICENSE",
            "xxhash": ROOT / "third_party/xxhash/LICENSE",
            "unicode": ROOT / "third_party/unicode_data/LICENSE",
            "zstd": ROOT / "third_party/zstd/LICENSE",
        }.items():
            copy_file(path, package, "share/vela/licenses/" + name + ".txt", "license", name, "included")
        support.update({"format_version": 1, "target": target, "runtime_abi": identity["runtime_abi"],
                        "toolchain_build": identity["toolchain_build"], "llvm_version": llvm_version,
                        "package_root": "../..", "runtime": runtime_relative,
                        "archiver": archiver, "linker": linker,
                        "acceptance": "development package; platform acceptance has not been certified"})
        (package / "share/vela/target.json").write_text(
            json.dumps(support, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
        verify_package(package, Path(temporary), environment)
        package.rename(destination)
        return environment


def main(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("variant", choices=BUILD_VARIANTS)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--support-spec", type=Path)
    parsed = parser.parse_args(arguments)
    output = configure(parsed.variant)
    runtime_name = static_library_name("vela_runtime")
    runtime = output / "obj/runtime" / runtime_name
    subprocess.run([str(toolchain_tools()["ninja"]), "-C", str(output),
                    executable_name("vela"), "obj/runtime/" + runtime_name], check=True)
    environment = create_package(output / executable_name("vela"), runtime,
                                 parsed.destination, parsed.support_spec)
    print(f"Created {parsed.destination.resolve()}")
    for key, value in environment.items():
        print(f"SDK configuration: {key}={value}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"package failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
