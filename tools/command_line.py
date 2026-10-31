"""Split developer command options from arguments forwarded to a child tool."""

from __future__ import annotations

import sys


def split_forwarded_arguments(arguments: list[str] | None) -> tuple[list[str], list[str]]:
    values = list(sys.argv[1:] if arguments is None else arguments)
    separator = values.index("--") if "--" in values else len(values)
    return values[:separator], values[separator + 1:]
