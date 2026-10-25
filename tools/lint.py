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


def _check_source(clang_tidy: Path, source: Path, *, fix: bool) -> LintResult:
    command = [
        str(clang_tidy), str(source), "-p", str(ROOT), "--quiet", "--warnings-as-errors=*",
    ]
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
    tools = toolchain_tools()
    build_files = _build_files(tools["gn"], parsed.variant)
    files = _path_files(parsed.paths, build_files) if parsed.paths else build_files
    if not files:
        raise RuntimeError("no C++ source files or headers were selected for lint")

    if parsed.fix:
        # 不并行修复：多个翻译单元可能同时改写共享头文件。
        results = [_check_source(tools["clang_tidy"], source, fix=True) for source in files]
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
                pool.submit(_check_source, tools["clang_tidy"], source, fix=False)
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
