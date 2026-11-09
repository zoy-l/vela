"""Report complete Vela collector cycles independently of language comparisons."""

from __future__ import annotations

import re
from statistics import median

from tools.benchmark_report import MICROSECONDS, iteration_rows, median_cells, validate_counters, variation


GC_CASE = re.compile(r"^gc/cohort/(\d+)/(\d+)/(\d+)/manual_time$")
COUNTERS = ("old_objects", "new_objects", "new_survivors", "start_ns", "p95_step_ns",
            "maximum_step_ns", "steps_per_cycle", "heap_before_bytes", "heap_live_bytes",
            "heap_committed_bytes", "reclaimed_bytes", "reclaimed_objects")


def gc_report(report: dict, expected_cases: list[str], repetitions: int) -> str:
    rows = iteration_rows(report, expected_cases, repetitions)
    cases = {}
    for name, entries in rows.items():
        match = GC_CASE.fullmatch(name)
        if not match:
            raise ValueError(f"{name}: not a collector cycle")
        old_objects, survivors, budget = map(int, match.groups())
        if budget == 0:
            raise ValueError(f"{name}: invalid step budget")
        for entry in entries:
            validate_counters(name, entry, COUNTERS)
            new_objects = entry["new_objects"]
            if (entry["old_objects"] != old_objects or entry["new_survivors"] != survivors or
                    new_objects != entries[0]["new_objects"] or new_objects != int(new_objects) or
                    entry["reclaimed_objects"] != new_objects - survivors):
                raise ValueError(f"{name}: incorrect cohort counts")
            if (entry["steps_per_cycle"] <= 0 or
                    entry["maximum_step_ns"] < entry["p95_step_ns"]):
                raise ValueError(f"{name}: invalid step measurements")
            if (entry["heap_before_bytes"] != entry["heap_live_bytes"] + entry["reclaimed_bytes"] or
                    entry["heap_committed_bytes"] < entry["heap_live_bytes"]):
                raise ValueError(f"{name}: inconsistent heap accounting")
        cases[old_objects, survivors, budget] = entries
    lines = ["Vela GC: complete incremental collection cycles, median wall microseconds.",
             f"All selected cases passed all {repetitions} repetitions and survivor checks.",
             "Objects are allocated through runtime Heap APIs. Automatic collection is disabled "
             "during preparation. Timing starts at StartCollection and ends after the final Step; "
             "input construction, percentile calculation and Isolate destruction are excluded. "
             "Per-step clock reads and sample recording remain in cycle time.",
             "Each cycle's step p95 is computed separately, then averaged within a repetition; "
             "the report takes the median of those averages. Max observed step is the maximum "
             "across all cycles and repetitions, and includes scheduling noise. Memory values are "
             "medians of the final cycle in each repetition, not process RSS or peak heap usage.",
             "Collector measurements have no C++ reference and are independent of AOT/JIT "
             "temporary-object workload ratios.", "",
             "| Existing objects | New objects | New survivors | Step budget | Cycle us | CV | Start ns | Mean cycle p95 step ns | Max observed step ns | Steps/cycle |",
             "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    ordered_cases = sorted(cases.items())
    for (old_objects, survivors, budget), entries in ordered_cases:
        times = [entry["real_time"] * MICROSECONDS[entry["time_unit"]] for entry in entries]
        values = median_cells(entries, ("start_ns", "p95_step_ns"), 1)
        maximum = max(entry["maximum_step_ns"] for entry in entries)
        steps = median(entry["steps_per_cycle"] for entry in entries)
        lines.append("| " + " | ".join([str(old_objects), f'{entries[0]["new_objects"]:.0f}',
                                        str(survivors), str(budget), f"{median(times):.3f}",
                                        variation(times), *values, f"{maximum:.1f}", f"{steps:.1f}"]) + " |")
    lines.extend(["", "| Existing objects | New survivors | Step budget | Heap before bytes | Live bytes | Committed bytes | Reclaimed bytes | Reclaimed objects |",
                  "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"])
    for (old_objects, survivors, budget), entries in ordered_cases:
        values = median_cells(entries, ("heap_before_bytes", "heap_live_bytes", "heap_committed_bytes",
                                       "reclaimed_bytes", "reclaimed_objects"), 0)
        lines.append("| " + " | ".join([str(old_objects), str(survivors), str(budget), *values]) + " |")
    return "\n".join(lines) + "\n"
