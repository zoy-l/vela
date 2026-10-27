"""Summarize selected execution backends, cold calls, compilation and memory separately."""

from __future__ import annotations

from collections import defaultdict
import math
import re
from statistics import mean, median, stdev


CASE = re.compile(
    r"^(compare|first|compile)/(cpp|vm|luajit|luajit_interp|representative)/"
    r"([^/]+)/(\d+)(?:/real_time)?$")
MICROSECONDS = {"ns": 0.001, "us": 1.0, "ms": 1000.0, "s": 1_000_000.0}
LABELS = {"cpp": "C++", "vm": "Vela", "luajit_interp": "Lua interpreter",
          "luajit": "LuaJIT", "representative": "Vela compile"}
NATIVE_CASE = re.compile(
    r"^native/(cpp|cpp_indirect|vm|vm_control)/(empty|integer|double|string_borrow)/(\d+)/real_time$")
NATIVE_BASELINES = {
    "dynamic_values": "Mixed arrays: C++ std::variant<int64_t, string, bool>; Vela dynamic[] with is checks and typed reads.",
    "dynamic_dispatch": "Bound calls: C++ std::function with a statically typed signature; Vela resolves a public method on a dynamic receiver and checks the int result. C++ has no runtime member-name lookup.",
    "json_roundtrip": "Construct, encode, decode and validate ordered records containing Unicode and mixed arrays. C++ uses pinned yyjson DOM arenas; Vela uses native Map/dynamic[] graphs, runtime checks and GC. The ratio includes these representation and contract differences, not only bytecode execution.",
    "byte_data_little": "Zeroed byte buffer, unaligned 64-bit stores/loads and an owner-alias check; C++ vector<byte>/memcpy with explicit little endian.",
    "byte_data_big": "Zeroed byte buffer, unaligned 64-bit stores/loads and an owner-alias check; C++ vector<byte>/memcpy with explicit big endian.",
    "regexp_records": "Both compile pinned PCRE2 with UTF/UCP, matching newline/BSR settings, interpreter budgets and no JIT. Build Unicode input, collect nonempty matches, convert coordinates to code points, copy captures and parse numbers inside timing.",
    "numeric_parse": "Format integers, parse decimal int/double values, verify the fraction and accumulate. C++ uses from_chars and pinned Abseil SimpleAtod; inputs are valid decimal strings.",
    "ascii_case": "Construct ASCII strings and verify upper/lower round trips. C++ uses ASCII character mapping; this workload makes no Unicode-casing comparison.",
    "utc_roundtrip": "Construct UTC timestamps, format canonical milliseconds, parse and inspect calendar fields. C++ uses pinned Abseil Time; inputs are valid canonical UTC strings, not the complete Vela error/range contract.",
}


def variation(values: list[float]) -> str:
    return f"{stdev(values) / mean(values):.1%}" if len(values) > 1 else "-"


def iteration_rows(report: dict, expected_cases: list[str], repetitions: int) -> dict[str, list[dict]]:
    expected = set(expected_cases)
    if not expected or repetitions < 1:
        raise ValueError("Comparison requires selected cases and positive repetitions")
    rows: dict[str, list[dict]] = defaultdict(list)
    for entry in report["benchmarks"]:
        if entry.get("error_occurred"):
            raise ValueError(f'{entry["name"]}: {entry.get("error_message", "benchmark failed")}')
        if entry.get("run_type", "iteration") == "iteration":
            rows[entry.get("run_name", entry["name"])].append(entry)
    if set(rows) != expected:
        raise ValueError(f"Comparison cases differ from registry: missing {sorted(expected - rows.keys())}; "
                         f"unexpected {sorted(rows.keys() - expected)}")
    for name, entries in rows.items():
        if (len(entries) != repetitions or
                any(entry.get("repetitions", 1) != repetitions for entry in entries) or
                {entry.get("repetition_index", 0) for entry in entries} != set(range(repetitions))):
            raise ValueError(f"{name}: incomplete or duplicate repetitions")
        times = [entry["real_time"] * MICROSECONDS[entry["time_unit"]] for entry in entries]
        if any(not math.isfinite(value) or value <= 0 for value in times):
            raise ValueError(f"{name}: invalid wall time")
    return rows


def comparison_report(report: dict, expected_cases: list[str], repetitions: int) -> str:
    if not expected_cases or any(CASE.fullmatch(name) is None for name in expected_cases):
        raise ValueError("No supported comparison cases selected")
    rows = iteration_rows(report, expected_cases, repetitions)
    samples: dict[tuple[str, str, str, int], list[float]] = {}
    counters: dict[tuple[str, str, int], dict[str, list[float]]] = {}
    for name, entries in rows.items():
        match = CASE.fullmatch(name)
        phase, backend, workload, size_text = match.groups()
        size = int(size_text)
        times = [entry["real_time"] * MICROSECONDS[entry["time_unit"]] for entry in entries]
        samples[phase, backend, workload, size] = times
        if phase != "compare" or backend == "cpp":
            continue
        fields = (("baseline_bytes", "allocated_bytes", "retained_bytes", "committed_bytes", "register_bytes")
                  if backend == "vm" else
                  ("baseline_bytes", "allocated_bytes", "retained_bytes", "warmup_calls",
                   "warm_traces", "traces", "warm_jit_code_bytes", "jit_code_bytes"))
        values = {}
        for field in fields:
            numbers = [entry[field] for entry in entries]
            if any(not math.isfinite(value) or value < 0 for value in numbers):
                raise ValueError(f"{name}: invalid {field}")
            values[field] = numbers
        if backend == "luajit" and any(value == 0 for value in values["warm_traces"]):
            raise ValueError(f"{name}: no compiled traces after warm-up")
        if backend == "luajit_interp" and any(
                value != 0 for field in ("warm_traces", "traces", "jit_code_bytes")
                for value in values[field]):
            raise ValueError(f"{name}: interpreter reported compiled traces")
        counters[backend, workload, size] = values

    lines = ["Median wall time per complete workload (microseconds).",
             "Ratios are Vela / the named backend; lower is better.",
             "Execution excludes source loading, bytecode compilation, input setup and warm-up; "
             "normal GC and subsequent JIT tracing remain timed.",
             f"Every selected case passed all {repetitions} repetitions.", ""]
    context = report.get("context", {})
    for key in sorted(context):
        if key.startswith("luajit_"):
            lines.append(f"{key}: {context[key]}")
    lines.append("")

    def table(phase: str, backends: tuple[str, ...], title: str) -> None:
        keys = sorted({(workload, size) for kind, _, workload, size in samples if kind == phase})
        if not keys:
            return
        lines.extend([title, ""])
        ratios = tuple(backend for backend in backends if backend != "vm") if phase != "compile" else ()
        columns = ["Workload", "Size", *(LABELS[backend] + " (us)" for backend in backends),
                   *("Vela/" + LABELS[backend] for backend in ratios),
                   "Samples " + "/".join(LABELS[backend] for backend in backends),
                   "CV " + "/".join(LABELS[backend] for backend in backends)]
        lines.extend(["| " + " | ".join(columns) + " |",
                      "| --- | " + " | ".join("---:" for _ in columns[1:]) + " |"])
        for workload, size in keys:
            values = {backend: samples.get((phase, backend, workload, size), []) for backend in backends}
            medians = {backend: median(times) for backend, times in values.items() if times}
            elapsed = [f"{medians[backend]:.3f}" if backend in medians else "-" for backend in backends]
            compared = [f'{medians["vm"] / medians[backend]:.2f}x'
                        if "vm" in medians and backend in medians else "-" for backend in ratios]
            counts = "/".join(str(len(values[backend])) for backend in backends)
            cvs = "/".join(variation(values[backend]) if values[backend] else "-" for backend in backends)
            lines.append("| " + " | ".join([workload, str(size), *elapsed, *compared, counts, cvs]) + " |")
        lines.append("")

    table("compare", ("cpp", "vm", "luajit_interp", "luajit"), "Steady execution after warm-up.")
    table("first", ("vm", "luajit_interp", "luajit"),
          "First invocation in a fresh state. Setup, compilation and destruction are excluded; "
          "LuaJIT tracing during this invocation is included.")
    table("compile", ("representative", "luajit"),
          "Compilation in a fresh state. Vela performs semantic analysis, optimization and bytecode "
          "preparation; LuaJIT loads source into Lua bytecode. These are distinct compiler boundaries.")

    lines.extend(["Heap snapshots in bytes (medians), including rooted input and program/library objects.",
                  "Retained is after full GC; these counters are not process RSS. "
                  "JIT code bytes sum live trace machine code, excluding unused reserved code pages.", "",
                  "| Workload | Size | Backend | Baseline | End heap | After GC | Committed | Registers | "
                  "Warm calls | Warm traces | Final traces | Warm JIT code | Final JIT code |",
                  "| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"])
    fields = ("baseline_bytes", "allocated_bytes", "retained_bytes", "committed_bytes", "register_bytes",
              "warmup_calls", "warm_traces", "traces", "warm_jit_code_bytes", "jit_code_bytes")
    for (backend, workload, size), values in sorted(counters.items(), key=lambda item: (item[0][1:],
                                                                                     item[0][0])):
        numbers = [f"{median(values[field]):.0f}" if field in values else "-" for field in fields]
        lines.append("| " + " | ".join([workload, str(size), LABELS[backend], *numbers]) + " |")
    notes = [(name, note) for name, note in NATIVE_BASELINES.items()
             if any(workload == name for _, _, workload, _ in samples)]
    if notes:
        lines.extend(["", "Native baseline contracts (workload-local construction and ordinary VM GC are timed; final collection is excluded).",
                      "C++ may inline or vectorize these implementations; ratios compare complete workloads.", ""])
        lines.extend(f"- `{name}`: {note}" for name, note in notes)
    return "\n".join(lines) + "\n"


def native_call_report(report: dict, expected_cases: list[str], repetitions: int) -> str:
    rows = iteration_rows(report, expected_cases, repetitions)
    samples = {}
    heaps = {}
    for name, entries in rows.items():
        match = NATIVE_CASE.fullmatch(name)
        if match is None:
            raise ValueError(f"Unsupported native comparison case: {name}")
        backend, workload, count_text = match.groups()
        count = int(count_text)
        if count <= 0 or any(entry["calls_per_iteration"] != count for entry in entries):
            raise ValueError(f"{name}: invalid call count")
        samples[backend, workload, count] = [
            entry["real_time"] * MICROSECONDS[entry["time_unit"]] * 1000 / count
            for entry in entries]
        if backend in {"vm", "vm_control"}:
            fields = ("baseline_heap_bytes", "end_heap_bytes", "baseline_heap_objects",
                      "end_heap_objects", "register_bytes")
            values = {field: [entry[field] for entry in entries] for field in fields}
            if any(not math.isfinite(value) or value < 0
                   for numbers in values.values() for value in numbers):
                raise ValueError(f"{name}: invalid heap or register counter")
            heaps[backend, workload, count] = values

    backends = ("cpp", "cpp_indirect", "vm_control", "vm")
    lines = ["Native call comparison: median wall nanoseconds per call/loop iteration.",
             f"Every selected case passed all {repetitions} repetitions.",
             "C++ uses the same non-inlined kernels and NativeContext as the VM. "
             "The indirect target is hidden from the compiler before timing.",
             "VM totals include loop work, checksum accumulation, native binding adapters and entry/exit "
             "amortized over the batch. Registration, compilation and warm-up are excluded.",
             "VM minus control subtracts independent medians, not paired samples. It estimates "
             "the additional native path including changed bytecode/register work, not an isolated "
             "ABI cost. The string control uses the known kernel result.", "",
             "| Workload | Calls/batch | C++ direct ns | C++ indirect ns | VM control ns | VM native ns | "
             "VM minus control ns | Samples direct/indirect/control/native | CV direct/indirect/control/native |",
             "| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |"]
    keys = sorted({(workload, count) for _, workload, count in samples})
    for workload, count in keys:
        values = [samples.get((backend, workload, count), []) for backend in backends]
        medians = [median(times) if times else None for times in values]
        elapsed = [f"{value:.3f}" if value is not None else "-" for value in medians]
        delta = (f"{medians[3] - medians[2]:.3f}"
                 if medians[3] is not None and medians[2] is not None else "-")
        counts = "/".join(str(len(times)) for times in values)
        cvs = "/".join(variation(times) if times else "-" for times in values)
        lines.append("| " + " | ".join([workload, str(count), *elapsed, delta, counts, cvs]) + " |")
    lines.extend(["", "VM GC heap snapshots and retained register capacity, in bytes/objects. "
                  "These are live snapshots, not cumulative allocation counts, host malloc counts "
                  "or process RSS.", "",
                  "| Workload | Calls/batch | Backend | Baseline heap bytes | End heap bytes | "
                  "Baseline objects | End objects | Register bytes |",
                  "| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |"])
    for (backend, workload, count), fields in sorted(heaps.items(), key=lambda item: (item[0][1:], item[0][0])):
        numbers = [f"{median(values):.0f}" for values in fields.values()]
        lines.append("| " + " | ".join([workload, str(count), backend, *numbers]) + " |")
    return "\n".join(lines) + "\n"
