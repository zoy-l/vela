"""Build LLVM Support against the embedding host's CRT allocation contract."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def members(archiver: Path, archive: Path) -> list[str]:
    result = subprocess.run([str(archiver), "t", str(archive)], check=True,
                            capture_output=True, text=True, encoding="utf-8")
    return result.stdout.splitlines()


def prepare(archiver: Path, source: Path, output: Path) -> None:
    original = members(archiver, source)
    overrides = [name for name in original if name.endswith("/rpmalloc/rpmalloc.c.obj")]
    if len(overrides) != 1:
        raise RuntimeError("The pinned Windows LLVM Support archive must contain one rpmalloc override")
    with tempfile.TemporaryDirectory(prefix="llvm-support-crt-", dir=output.parent) as work:
        candidate = Path(work) / output.name
        shutil.copyfile(source, candidate)
        subprocess.run([str(archiver), "dP", str(candidate), overrides[0]], check=True)
        candidate.replace(output)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archiver", required=True, type=Path)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    options = parser.parse_args()
    prepare(options.archiver, options.source, options.output)


if __name__ == "__main__":
    main()
