"""Summarize paired Google Benchmark samples without mixing timing boundaries."""

from __future__ import annotations

from collections import defaultdict
import math
import re
from statistics import mean, median, stdev


CASE = re.compile(r"^(compare/(cpp|vm)|compile/representative)/([^/]+)/(\d+)(?:/real_time)?$")
MICROSECONDS = {"ns": 0.001, "us": 1.0, "ms": 1000.0, "s": 1_000_000.0}


def variation(values: list[float]) -> str:
    return f"{stdev(values) / mean(values):.1%}" if len(values) > 1 else "-"


def comparison_report(report: dict) -> str:
    samples: dict[tuple[str, int], dict[str, list[float]]] = defaultdict(lambda: defaultdict(list))
    counters: dict[tuple[str, int], dict[str, list[float]]] = defaultdict(lambda: defaultdict(list))
    for entry in report["benchmarks"]:
        if entry.get("error_occurred"):
            raise ValueError(f'{entry["name"]}: {entry.get("error_message", "benchmark failed")}')
        if entry.get("run_type", "iteration") != "iteration":
            continue
        match = CASE.fullmatch(entry.get("run_name", entry["name"]))
        if match is None:
            continue
        group, backend, workload, size = match.groups()
        backend = "compile" if group == "compile/representative" else backend
        elapsed = entry["real_time"] * MICROSECONDS[entry["time_unit"]]
        if not math.isfinite(elapsed) or elapsed <= 0:
            raise ValueError(f'{entry["name"]}: invalid wall time')
        key = (workload, int(size))
        samples[key][backend].append(elapsed)
        if backend == "vm":
            for name in ("baseline_bytes", "retained_bytes", "committed_bytes", "register_bytes"):
                counters[key][name].append(entry[name])
    if not samples:
        raise ValueError("No paired comparison samples in the benchmark report")

    lines = [
        "Median wall time per complete workload; compilation and input preparation are excluded",
        "from execution. Ratio = Vela VM / optimized C++; lower is better. Compile is separate.",
        "",
        "| Workload | Size | Samples C++/VM | C++ (us) | Vela (us) | Ratio | CV C++/VM | Compile (us) |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for (workload, size), values in sorted(samples.items()):
        if not values["cpp"] or not values["vm"]:
            raise ValueError(f"{workload}/{size}: missing C++ or VM samples")
        native, vm = median(values["cpp"]), median(values["vm"])
        compile_time = f'{median(values["compile"]):.3f}' if values["compile"] else "-"
        lines.append(f'| {workload} | {size} | {len(values["cpp"])}/{len(values["vm"])} | '
                     f'{native:.3f} | {vm:.3f} | {vm / native:.2f}x | '
                     f'{variation(values["cpp"])}/{variation(values["vm"])} | {compile_time} |')
    lines.extend([
        "", "VM heap snapshots in bytes (medians; committed is an end-of-run snapshot, not a peak).",
        "Baseline and retained include the rooted input and program constants; retained is after GC.",
        "These are Vela heap counters, not process RSS or C++ allocator measurements.", "",
        "| Workload | Size | Baseline | After GC | Committed | Registers |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ])
    for (workload, size), values in sorted(counters.items()):
        numbers = [f'{median(values[name]):.0f}' for name in
                   ("baseline_bytes", "retained_bytes", "committed_bytes", "register_bytes")]
        lines.append(f'| {workload} | {size} | ' + ' | '.join(numbers) + ' |')
    return "\n".join(lines) + "\n"
