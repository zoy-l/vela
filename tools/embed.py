"""Export a relocatable SDK for native embedding."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

from tools.config import (
    BUILD_VARIANTS,
    MACOS_DEPLOYMENT_TARGET,
    ROOT,
    add_compiler_wrapper_argument,
    configure,
    executable_name,
    static_library_name,
    target_cpu,
    target_os,
    toolchain_tools,
)
from tools.package import (
    copy_file,
    describe_targets,
    package_bindgen,
    package_headers,
    package_licenses,
    target_closure,
)

RUNTIME_TARGET = "//runtime:vela_runtime"
DEVELOPMENT_TARGET = "//compiler:vela_development"
DEVELOPMENT_CLIENT_TARGET = "//compiler:vela_development_client"


def link_contract(targets: dict, root: str) -> tuple[list[str], list[str]]:
    libraries, frameworks = [], []
    for target in target_closure(targets, root):
        libraries.extend(target.get("libs", []))
        frameworks.extend(target.get("frameworks", []))
    return list(dict.fromkeys(libraries)), list(dict.fromkeys(frameworks))


def split_libraries(libraries: list[str]) -> tuple[list[Path], list[str]]:
    archives, system = [], []
    for library in libraries:
        path = Path(library)
        if library.startswith("//"):
            archives.append(ROOT / library.removeprefix("//"))
        elif path.is_absolute():
            archives.append(path)
        else:
            system.append(library)
    return list(dict.fromkeys(archives)), system


def package_embedding(output: Path, destination: Path, variant: str) -> None:
    targets = describe_targets(output)
    contracts = {}
    for name, target, library in (
            ("runtime", RUNTIME_TARGET, output / "obj/runtime" / static_library_name("vela_runtime")),
            ("development", DEVELOPMENT_TARGET,
             output / "obj/compiler" / static_library_name("vela_development")),
            ("development_client", DEVELOPMENT_CLIENT_TARGET,
             output / "obj/compiler" / static_library_name("vela_development_client"))):
        links, frameworks = link_contract(targets, target)
        dependencies, system = split_libraries(links)
        contracts[name] = ([library, *dependencies], system, frameworks)

    compiler = "bin/" + executable_name("vela")
    copy_file(output / executable_name("vela"), destination, compiler, "compiler", "Vela", "MIT")
    bindgen = package_bindgen(output / executable_name("vela_bindgen"), destination)
    package_headers(targets, DEVELOPMENT_TARGET, destination)
    package_licenses(destination)

    archives = {path: f"lib/{path.name}"
                for dependencies, _, _ in contracts.values() for path in dependencies}
    if len(set(archives.values())) != len(archives):
        raise RuntimeError("Embedding SDK archives must have distinct file names")
    for source, relative in archives.items():
        copy_file(source, destination, relative, "link", "Vela SDK", "share/vela/licenses")

    manifest = {
        "format_version": 5,
        "target_os": target_os(),
        "target_cpu": target_cpu(),
        "variant": variant,
        "macos_deployment_target": MACOS_DEPLOYMENT_TARGET if target_os() == "mac" else "",
        "compiler": compiler,
        "bindgen": {
            "command": bindgen,
            "manifest": "share/vela/bindgen.json",
        },
        "include_dirs": ["include"],
    }
    for name, (dependencies, system, frameworks) in contracts.items():
        manifest[name + "_libraries"] = [archives[path] for path in dependencies]
        manifest[name + "_system_libraries"] = system
        manifest[name + "_frameworks"] = frameworks
    (destination / "embedding.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")


def export(variant: str, destination: Path, cc_wrapper: str) -> Path:
    destination = destination.resolve()
    destination.mkdir(parents=True)
    try:
        output = configure(variant, cc_wrapper=cc_wrapper)
        subprocess.run(
            [str(toolchain_tools()["ninja"]), "-C", str(output), ":embedding"],
            cwd=ROOT, check=True,
        )
        package_embedding(output, destination, variant)
    except BaseException:
        shutil.rmtree(destination)
        raise
    return destination / "embedding.json"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("variant", choices=BUILD_VARIANTS)
    parser.add_argument("destination", type=Path)
    add_compiler_wrapper_argument(parser)
    options = parser.parse_args()
    print(f"Embedding SDK: {export(options.variant, options.destination, options.cc_wrapper)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"embed failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
