"""Build a native Vela distribution with pinned archive/link tools and audited inputs."""

from __future__ import annotations

import argparse
from collections.abc import Iterator
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile

from tools.config import BUILD_VARIANTS, MACOS_DEPLOYMENT_TARGET, ROOT, configure, executable_name, static_library_name, toolchain_tools
from tools.llvm import sdk_directory, sdk_specification
from tools.clang_sdk import resource_directory as clang_resource_directory


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
    # 缓存中的只读属性不属于发行契约；新包拥有自己的文件并允许更新及删除。
    destination.chmod(destination.stat().st_mode | stat.S_IWUSR)
    return {"path": "{package}/" + relative, "sha256": digest(destination), "role": role,
            "source": source_name, "license": license_name, "redistribution": "bundled"}


def describe_targets(output: Path) -> dict:
    described = command([toolchain_tools()["gn"], "desc", output, "//*", "--format=json"],
                        directory=ROOT)
    return json.loads(described.stdout)


def target_closure(targets: dict, root: str) -> Iterator[dict]:
    pending = [root]
    visited = set()
    while pending:
        label = pending.pop()
        if label in visited:
            continue
        visited.add(label)
        target = targets[label]
        pending.extend(target.get("deps", []))
        pending.extend(target.get("public_deps", []))
        yield target


def sdk_headers(targets: dict, root: str) -> list[Path]:
    headers = set()
    for target in target_closure(targets, root):
        for source in target.get("sources", []):
            if source.startswith(("//include/vela/", "//base/", "//runtime/", "//compiler/")):
                path = Path(source.removeprefix("//"))
                if path.suffix == ".h":
                    headers.add(path)
    return sorted(headers)


def package_headers(targets: dict, root: str, package: Path) -> None:
    headers = sdk_headers(targets, root)
    for path in headers:
        relative = path if path.parts[0] == "include" else Path("include") / path
        copy_file(ROOT / path, package, relative.as_posix(), "header", "Vela", "MIT")
    abseil = ROOT / "third_party/abseil-cpp"
    for path in (abseil / "absl").rglob("*"):
        if path.is_file() and path.suffix in {".h", ".inc"}:
            relative = (Path("include") / path.relative_to(abseil)).as_posix()
            copy_file(path, package, relative, "header", "Abseil", "Apache-2.0")
    # The development file watcher exposes the efsw declarations on POSIX.
    if Path("base/directory_watcher.h") in headers:
        copy_file(ROOT / "third_party/efsw/include/efsw/efsw.hpp", package,
                  "include/efsw/efsw.hpp", "header", "efsw", "MIT")


def package_licenses(package: Path) -> None:
    for name, path in {
        "vela": ROOT / "LICENSE", "llvm": sdk_directory() / "LICENSE.TXT",
        "abseil": ROOT / "third_party/abseil-cpp/LICENSE",
        "efsw": ROOT / "third_party/efsw/LICENSE",
        "pcre2": ROOT / "third_party/pcre2/LICENCE.md",
        "utf8proc": ROOT / "third_party/utf8proc/LICENSE.md",
        "yyjson": ROOT / "third_party/yyjson/LICENSE",
        "xxhash": ROOT / "third_party/xxhash/LICENSE",
        "unicode": ROOT / "third_party/unicode_data/LICENSE",
        "zlib": ROOT / "third_party/zlib/LICENSE",
        "zstd": ROOT / "third_party/zstd/LICENSE",
    }.items():
        copy_file(path, package, "share/vela/licenses/" + name + ".txt", "license", name, "included")


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
    """Exercise the packaged compiler, linker and runtime before reporting success."""
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


def package_bindgen(executable: Path, package: Path) -> str:
    resource = clang_resource_directory()
    paths = {
        "generator": "bin/" + executable_name("vela_bindgen"),
        "formatter": "libexec/" + executable_name("clang-format"),
        "style": "share/vela/bindings.clang-format",
        "resource_dir": "share/vela/clang/" + resource.name,
    }
    entry = "bin/vela-bindgen.py"
    inputs = {
        paths["generator"]: executable,
        paths["formatter"]: toolchain_tools()["clang_format"],
        paths["style"]: ROOT / ".clang-format",
        "tools/bindgen.py": ROOT / "tools/bindgen.py",
        entry: ROOT / "tools/bindgen_entry.py",
    }
    for path in resource.rglob("*"):
        if path.is_file():
            inputs[paths["resource_dir"] + "/" + path.relative_to(resource).as_posix()] = path
    for relative, source in inputs.items():
        owner = ("Vela" if source == executable or source.is_relative_to(ROOT / "tools")
                 or source == ROOT / ".clang-format" else "LLVM")
        license_name = "MIT" if owner == "Vela" else "Apache-2.0 WITH LLVM-exception"
        copy_file(source, package, relative, "bindgen", owner, license_name)
    manifest = {"format_version": 1, **paths,
                "files": {name: digest(package / name) for name in inputs}}
    (package / "share/vela/bindgen.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                                   encoding="utf-8", newline="\n")
    return entry


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
        linked = command([tools["linker"], "/nologo", "/out:" + str(work / "main.exe"),
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


def macos_support(runtime: Path, package: Path) -> tuple[dict, dict[str, str]]:
    """Record the selected Apple SDK and bundle the pinned compiler runtime."""
    tools = toolchain_tools()
    sdk = tools["sysroot"]
    settings = json.loads((sdk / "SDKSettings.json").read_text(encoding="utf-8"))
    version = settings["Version"]
    with tempfile.TemporaryDirectory(prefix="vela-macos-sdk-") as temporary:
        work = Path(temporary)
        source = work / "main.cc"
        source.write_text("int main() { return 0; }\n", encoding="utf-8", newline="\n")
        obj, application = work / "main.o", work / "main"
        command([tools["cxx"], "-c", source, "-o", obj, "-isysroot", sdk,
                 f"-mmacosx-version-min={MACOS_DEPLOYMENT_TARGET}"])
        driver = command([tools["cxx"], "-###", "-stdlib=libc++", "-fuse-ld=lld",
                          "--ld-path=" + str(tools["linker"]), "-isysroot", sdk,
                          f"-mmacosx-version-min={MACOS_DEPLOYMENT_TARGET}",
                          "-framework", "CoreFoundation", obj, runtime, "-o", application])
        invocation = shlex.split(driver.stderr.splitlines()[-1])[1:]
        platform = invocation.index("-platform_version")
        minimum = invocation[platform + 2]
        linked = command([tools["linker"], *invocation, "-t", "-force_load", runtime])
        files = []
        sdk_inputs = set()
        for line in linked.stdout.splitlines():
            match = re.fullmatch(r"(.*\.tbd)\(.*\)", line)
            path = Path(match[1] if match else line)
            if path.is_absolute() and path.is_relative_to(sdk) and path.is_file():
                sdk_inputs.add(path)
        if not sdk_inputs:
            raise RuntimeError("Pinned linker did not report Apple SDK input files")
        for path in sorted(sdk_inputs):
            files.append({"path": "{macos_sdk}/" + path.relative_to(sdk).as_posix(),
                          "sha256": digest(path), "role": "link", "source": "Apple macOS SDK",
                          "license": "Apple SDK license", "redistribution": "external SDK; not bundled"})
        replacements = {str(obj): "{object}", str(application): "{output}",
                        str(runtime): "{runtime}", str(sdk): "{macos_sdk}"}
        arguments = []
        llvm_root = sdk_directory()
        for value in invocation:
            if value in replacements:
                arguments.append(replacements[value])
                continue
            path = Path(value)
            if path.is_absolute():
                if not path.is_relative_to(llvm_root) or path.suffix != ".a":
                    raise RuntimeError(f"Undeclared macOS linker input: {value}")
                relative = "lib/clang/" + path.name
                files.append(copy_file(path, package, relative, "link", "LLVM",
                                       "Apache-2.0 WITH LLVM-exception"))
                arguments.append("{package}/" + relative)
            else:
                arguments.append(value)
        readobj = tools["cxx"].parent / executable_name("llvm-readobj")
        dependencies = command([readobj, "--needed-libs", application]).stdout
        libraries = dependencies.split("NeededLibraries [", 1)[1].split("]", 1)[0]
        return ({
            "external_roots": [{
                "name": "macos_sdk", "environment": "VELA_MACOS_SDK", "version": version,
                "source": "https://developer.apple.com/xcode/",
                "license": "Apple SDK license; SDK link inputs are not redistributed",
                "instructions": f"Install Apple macOS SDK {version} and set VELA_MACOS_SDK "
                                "to the absolute path returned by xcrun --sdk macosx --show-sdk-path",
            }],
            "files": files, "archive_arguments": ["rcs", "{output}", "{object}"],
            "link_arguments": arguments,
            "application_dependencies": sorted(line.strip() for line in libraries.splitlines()
                                               if line.strip()),
            "minimum_os": "macOS " + minimum + " (arm64)", "object_format": "Mach-O",
            "runtime_variant": "Apple system libc++; matching Vela runtime build identity",
        }, {"VELA_MACOS_SDK": str(sdk)})


def create_package(executable: Path, runtime: Path, destination: Path,
                   support_spec: Path | None = None) -> dict[str, str]:
    """Build and verify a new package at its final path, removing it on failure."""
    executable, runtime, destination = (path.resolve() for path in (executable, runtime, destination))
    if destination.exists():
        raise RuntimeError(f"Package destination already exists: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Windows readers can prevent moving a directory containing freshly executed tools.
    destination.mkdir()
    try:
        with tempfile.TemporaryDirectory(prefix=".vela-package-", dir=destination.parent) as temporary:
            workspace = Path(temporary)
            probe = workspace / "identity.vela"
            probe.write_text("export func identity(): int { return 0; }\n", encoding="utf-8", newline="\n")
            obj = workspace / "identity.o"
            command([executable, "build", probe, "--emit=obj", "--export=identity", "-o", obj])
            identity = json.loads(Path(str(obj) + ".aot.json").read_text(encoding="utf-8"))
            target = identity["target"]
            llvm_version = sdk_specification()[0]
            tools = toolchain_tools()
            if support_spec is not None:
                support = load_support_specification(support_spec, target)
                for bundled in support.pop("bundled_files", []):
                    support["files"].append(copy_file(
                        support_spec.parent / bundled["source_path"], destination, bundled["destination"],
                        "link", bundled["source"], bundled["license"]))
                environment = {}
            elif sys.platform == "win32":
                support, environment = windows_support(runtime)
            elif sys.platform == "darwin":
                support, environment = macos_support(runtime, destination)
            else:
                raise RuntimeError("This host requires an audited --support-spec with its SDK/sysroot, "
                                   "link inputs, licenses, minimum OS, and linker arguments")
            library_name = "vela_runtime.lib" if sys.platform == "win32" else "libvela_runtime.a"
            runtime_relative = f"lib/{target}/{library_name}"
            archiver = "libexec/" + tools["archiver"].name
            linker = "libexec/" + tools["linker"].name
            files = support["files"]
            for source, relative, role, owner, license_name in (
                    (executable, "bin/" + executable_name("vela"), "compiler", "Vela", "MIT"),
                    (runtime, runtime_relative, "runtime", "Vela and third-party runtime", "share/vela/licenses"),
                    (tools["archiver"], archiver, "archive", "project Clang toolchain", "Apache-2.0 WITH LLVM-exception"),
                    (tools["linker"], linker, "link", "project Clang toolchain", "Apache-2.0 WITH LLVM-exception")):
                files.append(copy_file(source, destination, relative, role, owner, license_name))
            package_headers(describe_targets(executable.parent), "//runtime:vela_runtime", destination)
            package_bindgen(executable.parent / executable_name("vela_bindgen"), destination)
            package_licenses(destination)
            support.update({"format_version": 1, "target": target, "runtime_abi": identity["runtime_abi"],
                            "toolchain_build": identity["toolchain_build"], "llvm_version": llvm_version,
                            "package_root": "../..", "runtime": runtime_relative,
                            "archiver": archiver, "linker": linker,
                            "acceptance": "development package; platform acceptance has not been certified"})
            (destination / "share/vela/target.json").write_text(
                json.dumps(support, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
            verify_package(destination, workspace, environment)
    except BaseException:
        shutil.rmtree(destination)
        raise
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
                    executable_name("vela"), executable_name("vela_bindgen"),
                    "obj/runtime/" + runtime_name], check=True)
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
