"""Validate native class benchmark coverage and summarize equivalent binding costs."""

from __future__ import annotations

import math
import statistics

from tools.benchmark_report import iteration_rows, MICROSECONDS


HOT = {"empty", "integer", "alternating", "floating", "boolean", "nullable", "void",
       "getter", "setter", "static", "repeated", "gc", "reentry"}


def aggregate_native_class_results(report: dict, expected: list[str], repetitions: int) -> dict:
    groups = iteration_rows(report, expected, repetitions)
    result = {}
    for name, rows in sorted(groups.items()):
        times = []
        for row in rows:
            elapsed = row["real_time"] * MICROSECONDS[row["time_unit"]] * 1000
            calls = row["calls_per_iteration"]
            if not math.isfinite(elapsed) or elapsed <= 0 or calls <= 0:
                raise ValueError(f"Invalid timing or batch count: {name}")
            times.append(elapsed / calls)
            workload = name.split("/")[-2]
            if "cpp_" not in name and workload in HOT and row["allocations_per_iteration"] != 0:
                raise ValueError(f"Hot host method allocated: {name}")
            if row.get("shared_owner_delta", 0) != 0:
                raise ValueError(f"Native class benchmark retained a shared owner: {name}")
        counters = {key: statistics.median([row[key] for row in rows]) for key in (
            "allocations_per_iteration", "constructed_per_iteration", "destroyed_per_iteration",
            "peak_live_bytes", "retained_bytes_delta", "shared_owner_delta", "instantiation_ns",
            "first_execution_ns", "entry_frame_bytes", "entry_shadow_slots", "entry_root_stores")
                    if key in rows[0]}
        result[name] = {"ns_per_call": statistics.median(times),
                        "stddev_ns": statistics.stdev(times) if len(times) > 1 else 0,
                        "min_ns": min(times), "max_ns": max(times), "counters": counters}
    for name, row in result.items():
        if "/class/" in name:
            free = result.get(name.replace("/class/", "/free/"))
            if free is None:
                raise ValueError(f"Missing equivalent free-function case: {name}")
            row["class_to_free"] = row["ns_per_call"] / free["ns_per_call"]
            row["difference_ns"] = row["ns_per_call"] - free["ns_per_call"]
            row["combined_stddev_ns"] = math.hypot(row["stddev_ns"], free["stddev_ns"])
    return result


def native_class_comparison_report(report: dict, expected: list[str], repetitions: int) -> str:
    summary = aggregate_native_class_results(report, expected, repetitions)
    lines = ["Native class bindings (Release)", "", "| Mode / workload | Class ns/call | Free ns/call | Ratio | Combined σ ns |",
             "| --- | ---: | ---: | ---: | ---: |"]
    for name, row in summary.items():
        if "/class/" not in name:
            continue
        free = summary[name.replace("/class/", "/free/")]
        label = name.removeprefix("native_class/").removesuffix("/real_time").replace("/class/", "/")
        lines.append(f"| {label} | {row['ns_per_call']:.3f} | {free['ns_per_call']:.3f} | "
                     f"{row['class_to_free']:.3f} | {row['combined_stddev_ns']:.3f} |")
    lines.append("")
    return "\n".join(lines)
