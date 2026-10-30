#!/usr/bin/env python3
"""Embed the SDK's authoritative Vela sources in the frontend binary."""

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    source = arguments.source.read_text(encoding="utf-8")
    delimiter = "VELA_SDK"
    if f'){delimiter}"' in source:
        raise ValueError("SDK source contains its embedding delimiter")
    output = "\n".join([
        "#pragma once",
        "#include <string_view>",
        "namespace vela::frontend::internal {",
        f'inline constexpr std::string_view kTestSource = R"{delimiter}({source}){delimiter}";',
        "}",
        "",
    ])
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(output, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
