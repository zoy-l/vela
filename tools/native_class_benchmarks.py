"""Generate paired native class/free-function workloads and their offline image."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from aot_benchmarks import compile_workloads


BATCH = 4096


def cases() -> list[dict]:
    return [
        {"name": "empty", "body": "object.Empty(); sum += index + 1;", "expected": BATCH * (BATCH + 1) // 2},
        {"name": "integer", "body": "sum += object.Integer(index);", "expected": BATCH * (BATCH + 1) // 2},
        {"name": "alternating", "before": "const item = index % 2 == 0 ? object : other;",
         "body": "sum += item.Integer(index);", "expected": BATCH * (BATCH + 2) // 2},
        {"name": "floating", "body": "sum += object.Floating(index.toDouble());",
         "result": "double", "expected": BATCH * (BATCH + 2) // 2},
        {"name": "boolean", "body": "sum += object.Boolean(index % 2 == 0) ? 1 : 0;", "expected": BATCH // 2},
        {"name": "nullable", "before": "let argument: Host? = null; if (index % 2 == 0) { argument = object; }",
         "body": "sum += object.Nullable(argument);", "expected": BATCH},
        {"name": "void", "body": "object.Write(index); sum += index;", "expected": BATCH * (BATCH - 1) // 2},
        {"name": "getter", "body": "sum += object.value;", "expected": BATCH},
        {"name": "setter", "body": "object.value = index; sum += index;", "expected": BATCH * (BATCH - 1) // 2},
        {"name": "static", "body": "sum += Host.Static(index);", "expected": BATCH * (BATCH + 1) // 2},
        {"name": "bound", "setup": "const call = object.Integer;", "body": "sum += call(index);",
         "expected": BATCH * (BATCH + 1) // 2},
        {"name": "dynamic", "setup": "const item: dynamic = object;", "body": "sum += item.Integer(index) as int;",
         "expected": BATCH * (BATCH + 1) // 2},
        {"name": "construct", "body": "const created = Host(initial: index); sum += created.value;",
         "count": 64, "expected": 64 * 63 // 2},
        {"name": "unique", "body": "const created = Host.Unique(index); if (created == null) { return -1; } sum += created.value;",
         "count": 64, "expected": 64 * 63 // 2},
        {"name": "shared", "body": "const created = Host.Shared(index); if (created == null) { return -1; } sum += created.value;",
         "count": 64, "expected": 64 * 63 // 2},
        {"name": "repeated", "body": "const created = Host.Repeated(); if (created == null || created != repeated) { return -1; } sum += created.value;",
         "expected": BATCH},
        {"name": "failure", "body": "try { object.Failure(true); } catch (error: Error) { sum += 1; }",
         "count": 16, "expected": 16},
        {"name": "gc", "body": "sum += object.Collect(index);", "count": 16, "expected": 16 * 17 // 2},
        {"name": "reentry", "body": "sum += object.Reenter(index);", "count": 64, "expected": 64 * 65 // 2},
    ]


DECLARATIONS = """import 'nativeclass';
external func freeEmpty(object: Host): void = 'classbench.empty';
external func freeInteger(object: Host, value: int): int = 'classbench.integer';
external func freeFloating(object: Host, value: double): double = 'classbench.floating';
external func freeBoolean(object: Host, value: bool): bool = 'classbench.boolean';
external func freeNullable(object: Host, other: Host?): int = 'classbench.nullable';
external func freeWrite(object: Host, value: int): void = 'classbench.void';
external func freeGet(object: Host): int = 'classbench.get';
external func freeSet(object: Host, value: int): void = 'classbench.set';
external func freeStatic(value: int): int = 'classbench.static';
external func freeConstruct(initial: int): Host = 'classbench.construct';
external func freeUnique(initial: int): Host? = 'classbench.unique';
external func freeShared(initial: int): Host? = 'classbench.shared';
external func freeRepeated(): Host? = 'classbench.repeated';
external func freeFailure(object: Host, fail: bool): int = 'classbench.failure';
external func freeCollect(object: Host, delta: int): int = 'classbench.gc';
external func freeReenter(object: Host, value: int): int = 'classbench.reentry';
let object = Host(initial: 1);
let other = Host(initial: 2);
let repeated = Host.Repeated();
export func receiver(): Host { return object; }
export func nested(value: int): int { return value + 1; }
"""


def free_source(text: str) -> str:
    replacements = {
        "object.Empty()": "freeEmpty(object)", "object.Integer(index)": "freeInteger(object, index)",
        "item.Integer(index)": "freeInteger(item, index)",
        "object.Floating(index.toDouble())": "freeFloating(object, index.toDouble())",
        "object.Boolean(index % 2 == 0)": "freeBoolean(object, index % 2 == 0)",
        "object.Nullable(argument)": "freeNullable(object, argument)",
        "object.Write(index)": "freeWrite(object, index)", "object.value = index": "freeSet(object, index)",
        "object.value": "freeGet(object)", "Host.Static(index)": "freeStatic(index)",
        "const call = object.Integer;": "const call = (value: int): int => freeInteger(object, value);",
        "const item: dynamic = object;": "const call: dynamic = freeInteger;",
        "Host(initial: index)": "freeConstruct(index)", "Host.Unique(index)": "freeUnique(index)",
        "Host.Shared(index)": "freeShared(index)", "Host.Repeated()": "freeRepeated()",
        "created.value": "freeGet(created)", "object.Failure(true)": "freeFailure(object, true)",
        "object.Collect(index)": "freeCollect(object, index)", "object.Reenter(index)": "freeReenter(object, index)",
    }
    for old, new in replacements.items():
        text = text.replace(old, new)
    return text


def generate(compiler: Path, output: Path) -> None:
    definitions = [DECLARATIONS]
    workloads = cases()
    exports = ["receiver", "nested"]
    for case in workloads:
        count = case.setdefault("count", BATCH)
        result = case.setdefault("result", "int")
        for path in ("class", "free"):
            name = path + "_" + case["name"]
            setup = case.get("setup", "")
            body = case.get("before", "") + case["body"]
            if path == "free":
                setup = free_source(setup)
                body = ("sum += call(object, index) as int;" if case["name"] == "dynamic"
                        else free_source(body))
            zero = "0.0" if result == "double" else "0"
            reset = "object.value = 1;" if path == "class" else "freeSet(object, 1);"
            definitions.append(f"export func {name}(): {result} {{ {reset} {setup} "
                               f"let sum = {zero}; for (let index = 0; index < {count}; index++) {{ "
                               + body + " } return sum; }\n")
            exports.append(name)
    source = output / "workloads.vela"
    source.write_text("\n".join(definitions), encoding="utf-8", newline="\n")
    manifest = compile_workloads(compiler, source, exports, output)
    indices = {entry["name"]: entry["function"] for entry in manifest["exports"]}
    header = ["#pragma once", "#include <array>", "#include <cstdint>", "#include <string_view>",
              '#include "runtime/native/native_image.h"',
              'extern "C" const vela::runtime::NativeImage* ' + manifest["image"] + "() noexcept;",
              "namespace vela::benchmarks::native_class {",
              "inline const auto* const kImage = " + manifest["image"] + "();",
              "inline constexpr std::uint32_t kReceiver = " + str(indices["receiver"]) + ";",
              "inline constexpr std::uint32_t kNested = " + str(indices["nested"]) + ";",
              "struct Case final { std::string_view name; std::int64_t count; double expected; "
              "bool floating; std::uint32_t class_function; std::uint32_t free_function; };",
              "inline constexpr auto kCases = std::to_array<Case>({"]
    for case in workloads:
        header.append("{.name = " + json.dumps(case["name"]) + ", .count = " + str(case["count"])
                      + ", .expected = " + str(case["expected"]) + ".0, .floating = "
                      + str(case["result"] == "double").lower() + ", .class_function = "
                      + str(indices["class_" + case["name"]]) + ", .free_function = "
                      + str(indices["free_" + case["name"]]) + "},")
    header += ["});", "}", ""]
    (output / "entries.h").write_text("\n".join(header), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    generate(arguments.compiler.resolve(), arguments.output.resolve())
