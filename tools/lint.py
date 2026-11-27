#!/usr/bin/env python3
"""Run the project-managed clang-tidy over Vela C++ sources and headers."""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from tools.build import describe_targets, target_closure
from tools.config import BUILD_VARIANTS, ROOT, configure, toolchain_tools


CPP_SUFFIXES = {".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx"}
HEADER_SUFFIXES = {".h", ".hpp", ".hxx"}


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


def is_project_file(path: Path) -> bool:
    return (
        path.is_relative_to(ROOT)
        and not path.is_relative_to(ROOT / "third_party")
        and not path.is_relative_to(ROOT / "out")
        and path.suffix in CPP_SUFFIXES
    )


def collect_build_files(targets: dict[str, dict]) -> list[Path]:
    files: set[Path] = set()
    for target in targets.values():
        for source in target.get("sources", []):
            path = (ROOT / source.removeprefix("//")).resolve()
            if is_project_file(path):
                files.add(path)
    return sorted(files)


def prepare_generated_headers(output: Path, ninja: Path, targets: dict[str, dict],
                              files: list[Path]) -> None:
    sources = {"//" + path.relative_to(ROOT).as_posix() for path in files}
    owners = [label for label, target in targets.items()
              if sources.intersection(target.get("sources", []))]
    headers = {
        (ROOT / name.removeprefix("//")).relative_to(output).as_posix()
        for target in target_closure(targets, *owners)
        for name in target.get("outputs", [])
        if name.endswith(".h") and "/gen/" in name
    }
    if headers:
        subprocess.run([str(ninja), "-C", str(output), *sorted(headers)],
                       cwd=ROOT, check=True)


def header_contexts(targets: dict[str, dict], files: list[Path],
                    compiler: Path) -> dict[Path, list[str]]:
    selected = {source for source in files if source.suffix in HEADER_SUFFIXES}
    contexts = {}
    clang_cl = compiler.stem == "clang-cl"
    define_prefix = "/D" if clang_cl else "-D"
    include_prefix = "/I" if clang_cl else "-I"
    for target in targets.values():
        owned = selected.intersection(
            (ROOT / source.removeprefix("//")).resolve()
            for source in target.get("sources", [])
        )
        if not owned:
            continue
        arguments = ["--driver-mode=cl", "/TP"] if clang_cl else ["-x", "c++"]
        arguments.extend(define_prefix + value for value in target.get("defines", []))
        arguments.extend(include_prefix + str((ROOT / value.removeprefix("//")).resolve())
                         for value in target.get("include_dirs", []))
        arguments.extend(target.get("cflags", []))
        arguments.extend(target.get("cflags_cc", []))
        for source in owned:
            if source in contexts and contexts[source] != arguments:
                raise RuntimeError(f"header has conflicting GN compilation contexts: {source}")
            contexts[source] = arguments
    missing = selected.difference(contexts)
    if missing:
        raise RuntimeError(f"header requires an owning GN target: {min(missing)}")
    return contexts


def path_files(paths: list[Path], build_files: list[Path]) -> list[Path]:
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


def check_source(clang_tidy: Path, source: Path, database: Path, *, fix: bool,
                 arguments: list[str] | None = None) -> LintResult:
    command = [
        str(clang_tidy), str(source), "--quiet", "--warnings-as-errors=*",
    ]
    if fix:
        command.extend(["--fix", "--format-style=file"])
    if arguments is None:
        command.extend(["-p", str(database)])
    else:
        command.extend(["--", *arguments])
    result = subprocess.run(
        command,
        cwd=ROOT if arguments is None else database,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    return LintResult(source=source, return_code=result.returncode, output=result.stdout)


def report_failure(result: LintResult) -> None:
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
    database = configure(parsed.variant)
    tools = toolchain_tools()
    targets = describe_targets(database)
    build_files = collect_build_files(targets)
    files = path_files(parsed.paths, build_files) if parsed.paths else build_files
    if not files:
        raise RuntimeError("no C++ source files or headers were selected for lint")
    prepare_generated_headers(database, tools["ninja"], targets, files)
    contexts = header_contexts(targets, files, tools["cxx"])

    if parsed.fix:
        # 不并行修复：多个翻译单元可能同时改写共享头文件。
        results = [check_source(tools["clang_tidy"], source, database, fix=True,
                                 arguments=contexts.get(source)) for source in files]
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
                pool.submit(check_source, tools["clang_tidy"], source, database, fix=False,
                            arguments=contexts.get(source))
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
        report_failure(result)
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
