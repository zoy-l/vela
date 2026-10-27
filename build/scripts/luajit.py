#!/usr/bin/env python3
"""Run pinned LuaJIT generators without writing into its source checkout."""

from __future__ import annotations

import argparse
import ast
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "third_party/luajit/src"


def run(*arguments: str | Path) -> None:
    subprocess.run([str(argument) for argument in arguments], check=True, cwd=SOURCE)


def generate_arch(options: argparse.Namespace) -> None:
    flags = (["/nologo", "/E", "/TC", "/clang:-dM"] if options.os == "win"
             else ["-E", "-dM", "-x", "c"])
    if options.sysroot:
        flags.extend(["-isysroot", options.sysroot])
    result = subprocess.run([options.cc, *flags, str(SOURCE / "lj_arch.h")],
                            capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stderr)
    macros = dict(re.findall(r"^#define (\w+)[ \t]+([^\n]+)$", result.stdout, re.MULTILINE))
    defines = []
    for macro, name in (("LJ_LE", "ENDIAN_LE"), ("LJ_HASJIT", "JIT"),
                        ("LJ_HASFFI", "FFI"), ("LJ_DUALNUM", "DUALNUM"),
                        ("LJ_ARCH_HASFPU", "FPU"), ("LJ_NO_UNWIND", "NO_UNWIND"),
                        ("LJ_ABI_PAUTH", "PAUTH"), ("LJ_ABI_BRANCH_TRACK", "BRANCH_TRACK"),
                        ("LJ_ABI_SHADOW_STACK", "SHADOW_STACK")):
        if macros.get(macro, "0").strip() == "1":
            defines.extend(["-D", name])
    if macros["LJ_ARCH_BITS"].strip() != "64":
        raise RuntimeError("LuaJIT benchmarks require the project's native 64-bit target")
    defines.extend(["-D", "P64"])
    if macros.get("LJ_ABI_SOFTFP", "0").strip() == "0":
        defines.extend(["-D", "HFABI"])
    if options.os == "win":
        defines.extend(["-D", "WIN"])
    defines.extend(["-D", "VER=" + macros.get("LJ_ARCH_VERSION", "0").strip()])
    arch = "arm64" if macros.get("LJ_TARGET_ARM64", "0").strip() == "1" else "x64"
    run(options.minilua, SOURCE.parent / "dynasm/dynasm.lua", "-LN", *defines,
        "-o", options.output, SOURCE / f"vm_{arch}.dasc")
    assignments = ast.parse((ROOT / "DEPS").read_text(encoding="utf-8")).body
    variables = next(ast.literal_eval(node.value) for node in assignments
                     if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name)
                     and node.targets[0].id == "vars")
    revision = variables["luajit_revision"]
    checkout = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=SOURCE, text=True).strip()
    if checkout != revision:
        raise RuntimeError("LuaJIT checkout differs from DEPS; run gclient sync --no-history")
    release = subprocess.check_output(["git", "show", "-s", "--format=%ct", revision],
                                      cwd=SOURCE, text=True).strip()
    directory = options.output.parent
    version = directory / "luajit_relver.txt"
    version.write_text(release + "\n", encoding="utf-8", newline="\n")
    run(options.minilua, SOURCE / "host/genversion.lua", SOURCE / "luajit_rolling.h",
        version, directory / "luajit.h")
    (directory / "luajit_build.h").write_text(
        '#pragma once\n#define VELA_LUAJIT_REVISION "' + revision + '"\n'
        + '#define VELA_LUAJIT_ARCH "' + arch + '"\n'
        + '#define VELA_LUAJIT_DUALNUM "' + macros["LJ_DUALNUM"].strip() + '"\n'
        + '#define VELA_LUAJIT_GC64 "' + macros["LJ_GC64"].strip() + '"\n',
        encoding="utf-8", newline="\n")


def generate_runtime(options: argparse.Namespace) -> None:
    directory = options.output
    for mode in ("bcdef", "ffdef", "libdef", "recdef"):
        run(options.buildvm, "-m", mode, "-o", directory / f"lj_{mode}.h", *options.library)
    run(options.buildvm, "-m", "folddef", "-o", directory / "lj_folddef.h", "lj_opt_fold.c")
    mode = {"win": "peobj", "mac": "machasm", "linux": "elfasm"}[options.os]
    run(options.buildvm, "-m", mode, "-o",
        directory / ("lj_vm.obj" if options.os == "win" else "lj_vm.S"))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("arch", "runtime"))
    parser.add_argument("--os", choices=("win", "mac", "linux"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cc")
    parser.add_argument("--sysroot")
    parser.add_argument("--minilua", type=Path)
    parser.add_argument("--buildvm", type=Path)
    parser.add_argument("--library", type=Path, action="append", default=[])
    options = parser.parse_args()
    options.output = options.output.resolve()
    options.minilua = options.minilua.resolve() if options.minilua else None
    options.buildvm = options.buildvm.resolve() if options.buildvm else None
    options.library = [path.resolve() for path in options.library]
    options.output.parent.mkdir(parents=True, exist_ok=True)
    if options.mode == "arch":
        generate_arch(options)
    else:
        options.output.mkdir(parents=True, exist_ok=True)
        generate_runtime(options)


if __name__ == "__main__":
    main()
