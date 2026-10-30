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


def aot_comparison_report(report: dict, expected_cases: list[str], repetitions: int) -> str:
    pattern = re.compile(r"^aot/(native|vm|cpp|first_native)/([^/]+)/(\d+)/real_time$")
    if not expected_cases or any(pattern.fullmatch(name) is None for name in expected_cases):
        raise ValueError("No supported AOT comparison cases selected")
    rows = iteration_rows(report, expected_cases, repetitions)
    samples = {}
    counters = {}
    residents = {}
    checksums = defaultdict(set)
    for name, entries in rows.items():
        backend, workload, size = pattern.fullmatch(name).groups()
        key = workload, int(size)
        samples[backend, *key] = [entry["real_time"] * MICROSECONDS[entry["time_unit"]] for entry in entries]
        if backend == "first_native":
            continue
        for entry in entries:
            result = entry["result"]
            expected = entry.get("expected_result", result)
            if (not math.isfinite(result) or not math.isfinite(expected) or
                    not math.isclose(result, expected, rel_tol=1e-12, abs_tol=1e-10)):
                raise ValueError(f"{name}: checksum mismatch")
            checksums[key].add(expected)
        resident = [entry["process_resident_bytes"] for entry in entries]
        if any(not math.isfinite(value) or value <= 0 for value in resident):
            raise ValueError(f"{name}: invalid process resident-memory counter")
        residents[backend, *key] = resident
        if backend == "cpp":
            continue
        fields = ("baseline_bytes", "allocated_bytes", "retained_bytes", "committed_bytes",
                  "suspended_frames", "saved_state_bytes", "remaining_suspended_frames", "remaining_saved_bytes")
        fields += (("entry_frame_bytes", "entry_shadow_slots", "entry_static_root_stores")
                   if backend == "native" else ("register_bytes",))
        values = {field: [entry[field] for entry in entries] for field in fields}
        if any(not math.isfinite(value) or value < 0 for numbers in values.values() for value in numbers):
            raise ValueError(f"{name}: invalid memory or suspended-state counter")
        for entry in entries:
            if (entry["baseline_bytes"] != entry["retained_bytes"] or
                    entry["remaining_suspended_frames"] != 0 or entry["remaining_saved_bytes"] != 0):
                raise ValueError(f"{name}: retained temporary state after completion and GC")
        counters[backend, *key] = values
    if any(len(values) != 1 for values in checksums.values()):
        raise ValueError("AOT, VM, and C++ checksums differ")
    lines = ["Offline AOT, VM and C++: median wall microseconds per workload.",
             f"Every selected case passed all {repetitions} repetitions and checksum checks.",
             "Steady execution excludes compilation, instantiation and warm-up. First native execution "
             "uses a fresh instance and includes lazy initialization, with instantiation excluded.",
             "C++ uses ordinary values, vector/string/unique_ptr ownership, and the same host kernel. "
             "Its async case queues two Future observation boundaries per element on the shared scheduler.",
             "Memory counters describe the Vela GC heap and saved continuations; they are not process RSS.", "",
             "| Workload | Size | AOT us | VM us | C++ us | First AOT us | AOT/VM | AOT/C++ | CV AOT/VM/C++ | VM/C++ |",
             "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | ---: |"]
    keys = sorted({(workload, size) for _, workload, size in samples})
    paired_ratios = []
    vm_ratios = []
    for workload, size in keys:
        values = {backend: samples.get((backend, workload, size), [])
                  for backend in ("native", "vm", "cpp", "first_native")}
        medians = {backend: median(times) for backend, times in values.items() if times}
        elapsed = [f"{medians[backend]:.3f}" if backend in medians else "-" for backend in values]
        ratios = [f"{medians['native'] / medians[backend]:.3f}" if "native" in medians and backend in medians
                  else "-" for backend in ("vm", "cpp")]
        cvs = "/".join(variation(values[backend]) if values[backend] else "-" for backend in ("native", "vm", "cpp"))
        vm_ratio = medians["vm"] / medians["cpp"] if "vm" in medians and "cpp" in medians else None
        lines.append("| " + " | ".join([workload, str(size), *elapsed, *ratios, cvs,
                                       f"{vm_ratio:.3f}" if vm_ratio is not None else "-"]) + " |")
        if "native" in medians and "cpp" in medians:
            paired_ratios.append(medians["native"] / medians["cpp"])
        if vm_ratio is not None:
            vm_ratios.append(vm_ratio)
    if paired_ratios:
        geometric_mean = math.exp(mean(math.log(value) for value in paired_ratios))
        lines.extend(["", f"Geometric mean AOT/C++ over {len(paired_ratios)} paired workloads: {geometric_mean:.2f}x."])
    if vm_ratios:
        geometric_mean = math.exp(mean(math.log(value) for value in vm_ratios))
        lines.extend(["", f"Geometric mean VM/C++ over {len(vm_ratios)} paired workloads: {geometric_mean:.2f}x."])
    if any(workload.startswith("dasprofile.") for workload, _ in keys):
        lines.extend(["", "The ported dasProfile workloads share Vela source between AOT and VM. Input preparation is excluded; "
                      "each timed call checks its result, and particle positions advance between calls. "
                      "Floating-point work uses double; sorting is stable on both sides. "
                      "Vela retains checked int64 arithmetic, managed containers and GC ownership. "
                      "SHA and iterative Fibonacci explicitly wrap at 32 bits."])
    lines.extend(["", "| Workload | Size | Backend | Baseline bytes | End bytes | Retained bytes | Committed bytes | Suspended frames | Saved bytes |",
                  "| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |"])
    for (backend, workload, size), fields in sorted(counters.items()):
        values = [f"{median(fields[field]):.0f}" for field in
                  ("baseline_bytes", "allocated_bytes", "retained_bytes", "committed_bytes", "suspended_frames", "saved_state_bytes")]
        lines.append("| " + " | ".join([workload, str(size), backend, *values]) + " |")
    lines.extend(["", "Native frame storage is the declared LLVM frame aggregate before optimization; "
                  "it is not measured machine-stack usage. Root stores count planned safepoint writes before LLVM optimization.", "",
                  "| Workload | Size | AOT frame storage bytes | AOT shadow slots | AOT static root stores | VM register bytes |",
                  "| --- | ---: | ---: | ---: | ---: | ---: |"])
    for workload, size in keys:
        columns = []
        for backend, field in (("native", "entry_frame_bytes"), ("native", "entry_shadow_slots"),
                               ("native", "entry_static_root_stores"), ("vm", "register_bytes")):
            values = counters.get((backend, workload, size), {}).get(field)
            columns.append(f"{median(values):.0f}" if values else "-")
        lines.append("| " + " | ".join([workload, str(size), *columns]) + " |")
    lines.extend(["", "Process resident snapshots include the complete comparison executable and prior "
                  "work in this process. Run one backend filter per process to compare resident costs. "
                  "These are not peak RSS or the footprint of a standalone AOT application.", "",
                  "| Workload | Size | Backend | Process resident bytes |",
                  "| --- | ---: | --- | ---: |"])
    for (backend, workload, size), values in sorted(residents.items()):
        lines.append("| " + " | ".join([workload, str(size), backend, f"{median(values):.0f}"]) + " |")
    return "\n".join(lines) + "\n"


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


def jit_comparison_report(report: dict, expected_cases: list[str], repetitions: int) -> str:
    pattern = re.compile(r"^(?:aot/(native|vm|cpp)|hot/steady/(baseline|optimized))/([^/]+)/(\d+)/real_time$")
    lifecycle_pattern = re.compile(
        r"^hot/(baseline|optimized)/(edits|closures|instances|continuations|rebuild|active|virtual)(?:/(\d+))?/manual_time$")
    rows = iteration_rows(report, expected_cases, repetitions)
    lifecycle = {}
    samples = {}
    counters = {}
    checksums = defaultdict(set)
    for name, entries in rows.items():
        match = pattern.fullmatch(name)
        if match is None:
            lifecycle_match = lifecycle_pattern.fullmatch(name)
            if lifecycle_match is None:
                raise ValueError(f"Unsupported JIT comparison case: {name}")
            for entry in entries:
                if entry["released_code_batches"] != 1 or entry["released_suspended_frames"] != 0:
                    raise ValueError(f"{name}: old code or suspended frames survived release")
                if (lifecycle_match[2] == "virtual" and
                        entry["materialized_objects"] != (1 if lifecycle_match[1] == "optimized" else 0)):
                    raise ValueError(f"{name}: incorrect materialized object count")
                for field in ("pause_ns", "pause_max_ns", "source_ns", "llvm_lowering_ns",
                              "llvm_optimization_ns", "machine_compilation_ns", "linking_ns",
                              "instantiation_ns", "validation_ns", "state_preparation_ns",
                              "deoptimization_ns", "commit_ns", "refresh_ns", "entry_ns",
                              "stopping_ns", "new_isolate_preparation_ns", "safepoint_waiting_ns",
                              "materialization_ns", "materialized_objects"):
                    value = entry[field]
                    if not math.isfinite(value) or value < 0:
                        raise ValueError(f"{name}: invalid {field}")
            lifecycle[name] = entries
            continue
        aot_backend, jit_backend, workload, size_text = match.groups()
        backend = aot_backend or jit_backend
        key = workload, int(size_text)
        samples[backend, *key] = [entry["real_time"] * MICROSECONDS[entry["time_unit"]]
                                 for entry in entries]
        counters[backend, *key] = entries
        for entry in entries:
            result = entry["result"]
            expected = entry.get("expected_result", result)
            if (not math.isfinite(result) or not math.isfinite(expected) or
                    not math.isclose(result, expected, rel_tol=1e-12, abs_tol=1e-10)):
                raise ValueError(f"{name}: checksum mismatch")
            checksums[key].add(expected)
            resident = entry["process_resident_bytes"]
            if not math.isfinite(resident) or resident <= 0:
                raise ValueError(f"{name}: invalid resident-memory counter")
            if backend == "cpp":
                continue
            if (entry["baseline_bytes"] != entry["retained_bytes"] or
                    entry["remaining_suspended_frames"] != 0 or entry["remaining_saved_bytes"] != 0):
                raise ValueError(f"{name}: retained temporary state")
            if backend in {"baseline", "optimized"}:
                for field in ("code_batches", "code_executable_bytes", "code_read_only_bytes"):
                    value = entry[field]
                    if not math.isfinite(value) or value <= 0:
                        raise ValueError(f"{name}: invalid {field}")
                for field in ("code_writable_bytes", "external_field_bytes"):
                    value = entry[field]
                    if not math.isfinite(value) or value < 0:
                        raise ValueError(f"{name}: invalid {field}")
    backends = ("cpp", "vm", "native", "baseline", "optimized")
    keys = sorted(checksums)
    for key in keys:
        if len(checksums[key]) != 1 or any((backend, *key) not in samples for backend in backends):
            raise ValueError(f"{key}: requires matching C++, VM, AOT, baseline and optimized JIT samples")
    lines = ["Reloadable JIT comparison: median wall microseconds per complete workload.",
             f"All selected cases passed all {repetitions} repetitions.",
             "Compilation and warm-up are excluded from steady timing. Named dispatch, precise roots, "
             "safepoints, external fields and ordinary GC remain enabled. Optimized code retains "
             "old-version baseline recovery; the closed AOT comparison uses its normal layout and call policy.",
             "Arithmetic that C++ simplifies as a whole is a separate case; its ratio does not predict JIT gains.", "",
             "| Workload | Size | C++ us | VM us | AOT us | Baseline us | Optimized us | Baseline/AOT | Optimized/AOT | CV C++/VM/AOT/baseline/optimized |",
             "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |"]
    ratios = {backend: [] for backend in ("baseline", "optimized")}
    for key in keys:
        values = {backend: samples[backend, *key] for backend in backends}
        medians = {backend: median(elapsed) for backend, elapsed in values.items()}
        for backend in ("baseline", "optimized"):
            ratios[backend].append(medians[backend] / medians["native"])
        lines.append("| " + " | ".join([key[0], str(key[1]),
                                        *[f"{medians[backend]:.3f}" for backend in backends],
                                        f'{medians["baseline"] / medians["native"]:.2f}x',
                                        f'{medians["optimized"] / medians["native"]:.2f}x',
                                        "/".join(variation(values[backend]) for backend in backends)]) + " |")
    for backend, values in ratios.items():
        if values:
            ratio = math.exp(mean(math.log(value) for value in values))
            lines.append(f"Geometric mean {backend} JIT/AOT over {len(keys)} paired workloads: {ratio:.2f}x.")
            selected = [value for key, value in zip(keys, values)
                        if key[0] not in {"arithmetic", "fields"}]
            if selected and len(selected) != len(values):
                ratio = math.exp(mean(math.log(value) for value in selected))
                lines.append(f"Geometric mean {backend} JIT/AOT excluding arithmetic and fields "
                             f"over {len(selected)} paired workloads: {ratio:.2f}x.")
    startup_fields = ("load_ns", "file_capture_ns", "engine_creation_ns", "source_total_ns", "code_total_ns",
                      "source_loading_ns",
                      "type_checking_ns", "typed_ir_ns", "llvm_lowering_ns", "llvm_optimization_ns",
                      "machine_compilation_ns", "linking_ns", "instantiation_ns",
                      "initial_publication_pause_ns", "first_execution_ns")
    lines.extend(["", "Fresh-session setup and first workload execution, median microseconds. "
                  "The process and OS file cache may already be warm; code caching is disabled. "
                  "Each JIT case compiles the entire source suite, outside steady timing. "
                  "dasProfile input preparation runs after loading and before first_execution_ns; "
                  "it is excluded from both first-workload and steady timing.", "",
                  "| Workload | Backend | " + " | ".join(field.removesuffix("_ns") + " us" for field in startup_fields) + " |",
                  "| --- | --- | " + " | ".join("---:" for _ in startup_fields) + " |"])
    for key in keys:
        for backend in ("baseline", "optimized"):
            entries = counters[backend, *key]
            numbers = [f"{median(entry[field] for entry in entries) / 1000:.3f}" for field in startup_fields]
            lines.append("| " + " | ".join([key[0], backend, *numbers]) + " |")
    lines.extend(["", "Memory is reported separately from timing. Linked code/image counters count block "
                  "contents, excluding allocator page padding and OS debugger overhead. Read-only image "
                  "contents include constants, source versions, location maps and unwind information. "
                  "Constant/debug payload counters describe logical image contents, including source "
                  "text and locations; they overlap the linked blocks and exclude allocator, string "
                  "deduplication and OS registration overhead. Process resident memory includes the "
                  "compiler and incremental source state. Resident samples share a process and may "
                  "include LLVM initialized by earlier cases; they are not independent backend footprints. "
                  "Compilation peak is the process lifetime "
                  "resident high-water mark sampled after compilation; run one case in a fresh process "
                  "to attribute it to that workload.", "",
                  "| Workload | Size | Backend | Heap after GC | External fields | Executable | Read-only image | Writable image | Code batches | Resident | Constant payload | Debug payload | Compilation process peak | VM registers |",
                  "| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"])
    fields = ("retained_bytes", "external_field_bytes", "code_executable_bytes", "code_read_only_bytes",
              "code_writable_bytes", "code_batches", "process_resident_bytes", "constant_payload_bytes",
              "debug_payload_bytes", "compilation_peak_resident_bytes", "register_bytes")
    for key in keys:
        for backend in backends:
            entries = counters[backend, *key]
            numbers = [f"{median(entry[field] for entry in entries):.0f}" if field in entries[0] else "-"
                       for field in fields]
            lines.append("| " + " | ".join([key[0], str(key[1]), backend, *numbers]) + " |")
    if lifecycle:
        fields = ("source_ns", "llvm_lowering_ns", "llvm_optimization_ns", "machine_compilation_ns",
                  "linking_ns", "safepoint_waiting_ns", "instantiation_ns", "validation_ns",
                  "materialization_ns", "state_preparation_ns", "deoptimization_ns", "commit_ns", "pause_ns", "pause_max_ns",
                  "new_isolate_preparation_ns", "stopping_ns", "entry_ns", "refresh_ns")
        lines.extend(["", "Reload lifecycle: wall microseconds per edit, including compilation. "
                      "Stage counters are per-edit averages; pause_max_ns records the largest complete "
                      "runtime pause in the sample. Instance expansion occurs on the first edit. "
                      "No framework refresh callback is installed, so refresh_ns is zero.", "",
                      "| Case | Total us | CV | Materialized objects | " + " | ".join(field.removesuffix("_ns") + " us" for field in fields) + " |",
                      "| --- | ---: | --- | ---: | " + " | ".join("---:" for _ in fields) + " |"])
        for name, entries in sorted(lifecycle.items()):
            times = [entry["real_time"] * MICROSECONDS[entry["time_unit"]] for entry in entries]
            numbers = [f"{median(entry[field] for entry in entries) / 1000:.3f}" for field in fields]
            materialized = median(entry["materialized_objects"] for entry in entries)
            lines.append("| " + " | ".join([name, f"{median(times):.3f}", variation(times),
                                            f"{materialized:.3f}", *numbers]) + " |")
        fields = ("heap_bytes", "external_field_bytes", "executable_bytes", "read_only_bytes",
                  "writable_bytes", "constant_payload_bytes", "debug_payload_bytes", "code_batches",
                  "suspended_frames", "saved_state_bytes")
        lines.extend(["", "Lifecycle memory snapshots after full GC, in bytes/counts. "
                      "Retained includes deliberately live old closures or continuations; released "
                      "drops them and verifies exactly one current code batch remains.", "",
                      "| Case | State | " + " | ".join(fields) + " |",
                      "| --- | --- | " + " | ".join("---:" for _ in fields) + " |"])
        for name, entries in sorted(lifecycle.items()):
            for phase in ("before", "retained", "released"):
                if phase + "_heap_bytes" not in entries[0]:
                    continue
                numbers = [f"{median(entry[phase + '_' + field] for entry in entries):.0f}" for field in fields]
                lines.append("| " + " | ".join([name, phase, *numbers]) + " |")
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
