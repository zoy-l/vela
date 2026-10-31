#!/usr/bin/env python3
"""Run the project-managed clang-tidy over Vela C++ sources and headers."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from tools.clangd import generate as generate_compilation_database
from tools.config import BUILD_VARIANTS, ROOT, output_directory, toolchain_tools


CPP_SUFFIXES = {".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx"}

# These fixed ABI signatures and fallible array owners are documented in .agents/cpp_style.md.
SCOPED_CHECK_EXCEPTIONS = {
    "compiler/jit/platform_support_win.cc": ("bugprone-easily-swappable-parameters",),
    "compiler/jit/code_resources.cc": ("bugprone-easily-swappable-parameters",),
    "runtime/native_operations.cc": ("bugprone-easily-swappable-parameters",),
    "runtime/native_instance.cc": ("cppcoreguidelines-avoid-c-arrays", "modernize-avoid-c-arrays"),
}


@dataclass(frozen=True)
class LintResult:
    source: Path
    return_code: int
    output: str


def parse_arguments(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path, metavar="PATH")
    parser.add_argument("--variant", choices=BUILD_VARIANTS, default="debug")
    parser.add_argument("--fix", action="store_true", help="Apply clang-tidy code fixes")
    parser.add_argument(
        "--jobs",
        type=int,
        default=6 if sys.platform == "darwin" else (os.cpu_count() or 1),
        help="Maximum concurrent clang-tidy processes (default: 6 on macOS, logical CPU count elsewhere)",
    )
    return parser.parse_args(arguments)


def _is_project_file(path: Path) -> bool:
    return (
        path.is_relative_to(ROOT)
        and not path.is_relative_to(ROOT / "third_party")
        and path.suffix in CPP_SUFFIXES
    )


def _build_files(gn: Path, variant: str) -> list[Path]:
    result = subprocess.run(
        [str(gn), "desc", str(output_directory(variant)), "//*", "sources", "--format=json"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    try:
        targets = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError("GN produced an invalid source listing") from error
    files: set[Path] = set()
    for target in targets.values():
        for source in target.get("sources", []):
            path = (ROOT / source.removeprefix("//")).resolve()
            if _is_project_file(path):
                files.add(path)
    return sorted(files)


def prepare_generated_headers(variant: str, tools: dict[str, Path], files: list[Path]) -> None:
    output = output_directory(variant)
    described = subprocess.run(
        [str(tools["gn"]), "desc", str(output), "//*", "--format=json"],
        cwd=ROOT, check=True, capture_output=True, text=True, encoding="utf-8",
    )
    try:
        targets = json.loads(described.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError("GN produced an invalid target description") from error
    sources = {"//" + path.relative_to(ROOT).as_posix() for path in files}
    pending = [label for label, target in targets.items()
               if sources.intersection(target.get("sources", []))]
    required: set[str] = set()
    while pending:
        label = pending.pop()
        if label in required:
            continue
        required.add(label)
        target = targets[label]
        pending.extend(target.get("deps", []))
        pending.extend(target.get("public_deps", []))
    headers = {
        (ROOT / name.removeprefix("//")).relative_to(output).as_posix()
        for label in required
        for name in targets[label].get("outputs", [])
        if name.endswith(".h") and "/gen/" in name
    }
    if headers:
        subprocess.run([str(tools["ninja"]), "-C", str(output), *sorted(headers)],
                       cwd=ROOT, check=True)


def _path_files(paths: list[Path], build_files: list[Path]) -> list[Path]:
    files: set[Path] = set()
    for path in paths:
        candidate = (path if path.is_absolute() else Path.cwd() / path).resolve()
        if not candidate.exists():
            raise RuntimeError(f"lint path does not exist: {candidate}")
        if not candidate.is_relative_to(ROOT) or candidate.is_relative_to(ROOT / "third_party"):
            raise RuntimeError(f"lint only checks project-owned C++ files: {candidate}")
        if candidate.is_file():
            if candidate.suffix not in CPP_SUFFIXES:
                raise RuntimeError(f"lint path is not a C++ source or header: {candidate}")
            files.add(candidate)
            continue
        files.update(source for source in build_files if source.is_relative_to(candidate))
    return sorted(files)


def _check_source(clang_tidy: Path, source: Path, database: Path, *, fix: bool) -> LintResult:
    command = [
        str(clang_tidy), str(source), "-p", str(database), "--quiet", "--warnings-as-errors=*",
    ]
    exceptions = SCOPED_CHECK_EXCEPTIONS.get(source.relative_to(ROOT).as_posix(), ())
    if exceptions:
        command.append("--checks=" + ",".join("-" + check for check in exceptions))
    if fix:
        command.extend(["--fix", "--format-style=file"])
    result = subprocess.run(
        command,
        cwd=ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    return LintResult(source=source, return_code=result.returncode, output=result.stdout)


def _report_failure(result: LintResult) -> None:
    print(
        f"clang-tidy failed for {result.source.relative_to(ROOT)} "
        f"(exit {result.return_code})",
        file=sys.stderr,
    )
    if result.output:
        print(result.output.rstrip(), file=sys.stderr)


def run(parsed: argparse.Namespace) -> int:
    if parsed.jobs < 1:
        raise RuntimeError("--jobs must be greater than zero")
    generate_compilation_database(parsed.variant)
    database = output_directory(parsed.variant)
    tools = toolchain_tools()
    build_files = _build_files(tools["gn"], parsed.variant)
    files = _path_files(parsed.paths, build_files) if parsed.paths else build_files
    if not files:
        raise RuntimeError("no C++ source files or headers were selected for lint")
    prepare_generated_headers(parsed.variant, tools, files)

    if parsed.fix:
        # 不并行修复：多个翻译单元可能同时改写共享头文件。
        results = [_check_source(tools["clang_tidy"], source, database, fix=True) for source in files]
    else:
        worker_count = min(parsed.jobs, len(files))
        print(
            f"clang-tidy: {len(files)} C++ files with {worker_count} jobs",
            flush=True,
        )
        started = time.monotonic()
        results = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=worker_count) as pool:
            pending = {
                pool.submit(_check_source, tools["clang_tidy"], source, database, fix=False)
                for source in files
            }
            completed = 0
            while pending:
                done, pending = concurrent.futures.wait(
                    pending,
                    timeout=5,
                    return_when=concurrent.futures.FIRST_COMPLETED,
                )
                elapsed = time.monotonic() - started
                if not done:
                    print(
                        f"clang-tidy: [{completed}/{len(files)}] "
                        f"{completed / len(files):.0%}, {elapsed:.1f}s elapsed; "
                        f"waiting for {len(pending)} remaining files",
                        flush=True,
                    )
                for future in done:
                    result = future.result()
                    results.append(result)
                    completed += 1
                    print(
                        f"clang-tidy: [{completed}/{len(files)}] "
                        f"{completed / len(files):.0%}, {elapsed:.1f}s elapsed; "
                        f"{'FAIL' if result.return_code else 'OK'} "
                        f"{result.source.relative_to(ROOT).as_posix()}",
                        flush=True,
                    )

    failures = [result for result in results if result.return_code != 0]
    for result in failures:
        _report_failure(result)
    print(f"linted {len(files)} C++ file(s), including headers; failures: {len(failures)}")
    return 1 if failures else 0


def main(arguments: list[str] | None = None) -> int:
    parsed = parse_arguments(arguments)
    return run(parsed)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"lint failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
