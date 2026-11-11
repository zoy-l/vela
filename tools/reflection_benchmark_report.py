"""Validate reflection measurements and compare lookup, dispatch and materialization costs."""

from __future__ import annotations

import re
from statistics import mean, median, stdev

from tools.benchmark_report import checked_result, iteration_rows, MICROSECONDS, validate_counters


OPERATION = re.compile(
    r"^reflection/(aot|closed|baseline|optimized|cpp)/(field|call|construct|lookup|metadata|unused)/"
    r"([a-z_]+)/real_time$")
QUERY = re.compile(
    r"^reflection/(aot|closed|baseline|optimized)/query/(all|module|missing)/"
    r"background:(\d+)/matches:(\d+)/real_time$")
RELOAD = "reflection/reload/resources/iterations:1/real_time"
HEAP_COUNTERS = ("allocation_bytes_per_op", "baseline_bytes", "allocated_bytes", "retained_bytes",
                 "committed_bytes", "reflection_types", "reflection_members",
                 "reflection_index_prepared")


def aggregate_reflection_results(report: dict, expected: list[str], repetitions: int) -> dict:
    groups = iteration_rows(report, expected, repetitions)
    summary = {}
    for name, rows in sorted(groups.items()):
        operation = OPERATION.fullmatch(name)
        query = QUERY.fullmatch(name)
        if not operation and not query and name != RELOAD:
            raise ValueError(f"Unsupported reflection case: {name}")
        backend = query[1] if query else operation[1] if operation else None
        script = backend is not None and backend != "cpp"
        aot = backend == "aot"
        batch = 8 if query else 16 if name == RELOAD else 128
        checksum = batch * (batch - 1) // 2 if operation else batch
        if operation and operation[2] in {"lookup", "metadata"}:
            checksum = batch
        if query:
            checksum = batch * (0 if query[2] == "missing" else int(query[4]))
        counter_names = ["operations", "result", "expected_result"]
        if script:
            counter_names += HEAP_COUNTERS
            counter_names += ["aot_object_bytes"] if aot else ["image_bytes", "executable_bytes"]
        if query:
            counter_names += ["background_members", "matches_per_query"]
        if name == RELOAD:
            counter_names += ["retained_images", "image_bytes", "executable_bytes"]
        times = []
        for row in rows:
            validate_counters(name, row, tuple(counter_names))
            if aot and row["aot_object_bytes"] <= 0:
                raise ValueError(f"{name}: invalid AOT object size")
            if row["operations"] != batch:
                raise ValueError(f"{name}: invalid operations per batch")
            if checked_result(name, row) != checksum:
                raise ValueError(f"{name}: unexpected workload checksum")
            if query and (row["background_members"] != int(query[3]) or
                          row["matches_per_query"] != checksum / batch):
                raise ValueError(f"{name}: invalid query cardinality")
            if name == RELOAD and row["retained_images"] != 1:
                raise ValueError(f"{name}: retired code image retained")
            if script:
                registered = not operation or operation[3] not in {"direct", "dynamic"}
                queried = registered and (not operation or operation[2] != "unused")
                if row["reflection_index_prepared"] != int(queried):
                    raise ValueError(f"{name}: unexpected runtime reflection index")
                if not registered and row["reflection_members"] != 0:
                    raise ValueError(f"{name}: unregistered control contains reflected members")
            times.append(row["real_time"] * MICROSECONDS[row["time_unit"]] * 1000 / batch)
        summary[name] = {
            "operation": operation.groups() if operation else None,
            "query": query.groups() if query else None,
            "ns_per_operation": median(times),
            "wall_cv": stdev(times) / mean(times) if len(times) > 1 else 0,
            "min_ns": min(times), "max_ns": max(times), "samples": len(times),
            "counters": {key: median(row[key] for row in rows) for key in counter_names},
        }
    for name, result in summary.items():
        match = result["operation"]
        if not match:
            continue
        backend, workload, strategy = match
        controls = {}
        if workload in {"field", "call", "construct"}:
            controls["direct"] = f"reflection/{backend}/{workload}/direct/real_time"
        if workload in {"field", "call"} and backend != "cpp":
            controls["dynamic"] = f"reflection/{backend}/{workload}/dynamic/real_time"
            controls["cpp_reference"] = f"reflection/cpp/{workload}/{strategy}/real_time"
        if workload == "unused":
            controls["direct"] = f"reflection/{backend}/field/direct/real_time"
        result["ratios"] = {key: result["ns_per_operation"] / summary[control]["ns_per_operation"]
                            for key, control in controls.items() if control in summary}
    return summary


def reflection_comparison_report(report: dict, expected: list[str], repetitions: int) -> str:
    summary = aggregate_reflection_results(report, expected, repetitions)
    lines = ["Runtime reflection (Release)", "",
             "Medians and sample CV use wall time per logical operation. A field operation writes then reads; "
             "a call takes one int and returns it. Each batch includes the loop and checksum check. "
             "Vela also includes one host entry per 128 operations (8 for queries); no overhead is subtracted.", "",
             "Compilation, instantiation, input preparation, verification and warm-up are excluded. "
             "Immutable images are shared; every sample uses a fresh Isolate. closed is closed-program "
             "JIT; aot links offline images emitted by vela build --emit=obj. All four modes use the same "
             "generated source. baseline and optimized use SourceSession reloadable images and published named entries. "
             "Automatic GC stays timed; final forced collection is excluded. Managed bytes/op count "
             "Vela Heap allocations, including accounted external blocks, and do not measure C++ allocations.", "",
             "cached reuses descriptors and CallArguments; packed creates arguments each call. "
             "member_lookup and type_member_lookup find descriptors and pack arguments each operation. "
             "Direct controls retain normal inlining and redundant-access elimination. unused/registered_direct "
             "adds only [Reflect] to the direct source; it contains no reflection API entry points. "
             "Registered images prepare their index during instantiation; heap and image counters record "
             "that cost. Unregistered controls must have no index or member directory.", "",
             "C++ reference uses hand-registered hash tables, accessor/invoke function pointers, tagged "
             "int values and success/error checks. Its Counter& receiver and one positional int signature "
             "are fixed; it has no Vela GC, reload lifecycle, named/default arguments or general runtime "
             "type checks. C++ ratios include these contract differences. The C++ host reflection API is not measured.", "",
             "| Backend / operation / strategy | ns/op | CV | / direct | / dynamic | / C++ reference | Managed B/op |",
             "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for name, result in summary.items():
        if not result["operation"]:
            continue
        label = name.removeprefix("reflection/").removesuffix("/real_time")
        ratios = result["ratios"]
        cells = [f"{ratios[key]:.2f}x" if key in ratios else "-"
                 for key in ("direct", "dynamic", "cpp_reference")]
        allocation = result["counters"].get("allocation_bytes_per_op")
        allocated = f"{allocation:.0f}" if allocation is not None else "-"
        lines.append(f"| {label} | {result['ns_per_operation']:.3f} | {result['wall_cv']:.1%} | "
                     + " | ".join(cells) + f" | {allocated} |")
    queries = {name: result for name, result in summary.items() if result["query"]}
    if queries:
        lines += ["", "findMethods<Button>() includes index lookup, a new result array, method descriptors, "
                  "match records and independent attribute data copies. Timed consumption reads the count; "
                  "an untimed verifier checks method order, invocation results and copy independence. "
                  "Background members are registered methods without Button annotations. "
                  "module selects targets.vela; missing selects a nonexistent module.", "",
                  "| Backend / query | Background | Matches | ns/query | CV | Managed B/query |",
                  "| --- | ---: | ---: | ---: | ---: | ---: |"]
        for name, result in queries.items():
            backend, mode, _, _ = result["query"]
            counters = result["counters"]
            lines.append(f"| {backend} / {mode} | {int(counters['background_members'])} | "
                         f"{int(counters['matches_per_query'])} | {result['ns_per_operation']:.3f} | "
                         f"{result['wall_cv']:.1%} | {counters['allocation_bytes_per_op']:.0f} |")
    aot_cases = {name: result for name, result in summary.items()
                 if "aot_object_bytes" in result["counters"]}
    if aot_cases:
        lines += ["", "Offline AOT object file bytes are reported separately from timing and managed "
                  "allocations. Cases sharing a source program share one linked object; repeated sizes "
                  "must not be summed. Object files contain code, metadata and relocation records; "
                  "they are not JIT executable-memory or resident-memory measurements.", "",
                  "| AOT case | Object bytes |", "| --- | ---: |"]
        for name, result in aot_cases.items():
            label = name.removeprefix("reflection/aot/").removesuffix("/real_time")
            lines.append(f"| {label} | {result['counters']['aot_object_bytes']:.0f} |")
    if RELOAD in summary:
        result = summary[RELOAD]
        lines += ["", f"Reload resources: {result['ns_per_operation'] / 1_000_000:.3f} ms/source publication "
                  f"(CV {result['wall_cv']:.1%}). Each sample submits 16 source publications "
                  "(the first repeats the initial source), invokes an old "
                  "method handle against each new publication and forces GC; only one image may remain. "
                  "This includes compilation, publication, invocation and collection, unlike steady operations."]
    return "\n".join(lines) + "\n"
