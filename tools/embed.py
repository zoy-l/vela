"""Build local third-party embedding libraries without copying sources into a host."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from tools.config import (
    BUILD_VARIANTS, MACOS_DEPLOYMENT_TARGET, ROOT, add_compiler_wrapper_argument, configure, executable_name,
    static_library_name, target_cpu, target_os, toolchain_tools,
)
from tools.llvm import link_configuration


def link_contract(output: Path, root: str) -> tuple[list[str], list[str]]:
    described = subprocess.run(
        [str(toolchain_tools()["gn"]), "desc", str(output), "//*", "--format=json"],
        cwd=ROOT, check=True, capture_output=True, text=True, encoding="utf-8",
    )
    targets = json.loads(described.stdout)
    pending, visited = [root], set()
    libraries, frameworks = [], []
    while pending:
        label = pending.pop()
        if label in visited:
            continue
        visited.add(label)
        target = targets[label]
        libraries.extend(target.get("libs", []))
        frameworks.extend(target.get("frameworks", []))
        pending.extend(target.get("deps", []))
        pending.extend(target.get("public_deps", []))
    return list(dict.fromkeys(libraries)), list(dict.fromkeys(frameworks))


def split_libraries(libraries: list[str]) -> tuple[list[str], list[str]]:
    archives, system = [], []
    for library in libraries:
        if library.startswith("//"):
            archives.append((ROOT / library.removeprefix("//")).as_posix())
        elif Path(library).is_absolute():
            archives.append(library)
        else:
            system.append(library)
    return archives, system


def export(variant: str, cc_wrapper: str) -> Path:
    output = configure(variant, cc_wrapper=cc_wrapper)
    subprocess.run(
        [str(toolchain_tools()["ninja"]), "-C", str(output), ":embedding"],
        cwd=ROOT, check=True,
    )
    llvm = link_configuration()
    runtime = output / "obj/runtime" / static_library_name("vela_runtime")
    development = output / "obj/compiler" / static_library_name("vela_development")
    runtime_link, runtime_frameworks = link_contract(output, "//runtime:vela_runtime")
    development_link, development_frameworks = link_contract(output, "//compiler:vela_development")
    runtime_archives, runtime_system = split_libraries(runtime_link)
    development_archives, development_system = split_libraries(development_link)
    # 本地开发描述借用 checkout 的公开头；库仍由 Vela 自己的工具链独立构建。
    includes = [ROOT, ROOT / "include", ROOT / "third_party/abseil-cpp",
                ROOT / "third_party/efsw/include", output / "gen"]
    manifest = {
        "format_version": 2,
        "target_os": target_os(),
        "target_cpu": target_cpu(),
        "variant": variant,
        "macos_deployment_target": MACOS_DEPLOYMENT_TARGET if target_os() == "mac" else "",
        "compiler": str(output / executable_name("vela")),
        "linker": toolchain_tools()["linker"].as_posix(),
        "include_dirs": [path.as_posix() for path in includes],
        "runtime_libraries": [runtime.as_posix(), *runtime_archives],
        "runtime_system_libraries": runtime_system,
        "runtime_frameworks": runtime_frameworks,
        "development_libraries": list(dict.fromkeys([
            development.as_posix(), *llvm["vela_llvm_jit_libraries"],
            *llvm["vela_llvm_libraries"],
            *development_archives,
        ])),
        "development_system_libraries": list(dict.fromkeys([
            *llvm["vela_llvm_jit_system_libraries"], *llvm["vela_llvm_system_libraries"],
            *development_system,
        ])),
        "development_frameworks": development_frameworks,
    }
    destination = output / "embedding.json"
    content = json.dumps(manifest, indent=2) + "\n"
    if not destination.is_file() or destination.read_text(encoding="utf-8") != content:
        destination.write_text(content, encoding="utf-8", newline="\n")
    return destination


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("variant", choices=BUILD_VARIANTS, default="release", nargs="?")
    add_compiler_wrapper_argument(parser)
    options = parser.parse_args()
    print(f"Embedding libraries: {export(options.variant, options.cc_wrapper)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"embed failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
