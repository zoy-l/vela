"""Validate and summarize the complete escape optimization comparison."""

from __future__ import annotations

import math
import re
from statistics import mean, median, stdev

from tools.benchmark_report import MICROSECONDS, iteration_rows


CASE = re.compile(r"^escape/(heap|scalar|borrow)/([^/]+)/real_time$")
RELOAD = re.compile(r"^hot/(baseline|optimized)/virtual/manual_time$")
MODES = ("heap", "scalar", "borrow")


def aggregate_escape_results(report: dict, expected_cases: list[str], repetitions: int) -> dict:
    rows = iteration_rows(report, expected_cases, repetitions)
    workloads: dict[str, set[str]] = {}
    result = {}
    for name, entries in rows.items():
        match = CASE.fullmatch(name)
        reload = RELOAD.fullmatch(name)
        if not match and not reload:
            raise ValueError(f"Unsupported escape benchmark: {name}")
        if match:
            mode, workload = match.groups()
            workloads.setdefault(workload, set()).add(mode)
            for entry in entries:
                if entry["result"] != entry["expected_result"]:
                    raise ValueError(f"{name}: checksum mismatch")
        else:
            for entry in entries:
                if (entry["released_code_batches"] != 1 or entry["released_saved_state_bytes"] != 0 or
                        entry["released_suspended_frames"] != 0 or
                        entry["materialized_objects"] != (1 if reload[1] == "optimized" else 0)):
                    raise ValueError(f"{name}: incomplete materialization or code retirement")
        times = [entry["real_time"] * MICROSECONDS[entry["time_unit"]] * 1000 for entry in entries]
        ignored = {"name", "run_name", "family_index", "per_family_instance_index", "run_type",
                   "repetitions", "repetition_index", "threads", "iterations", "real_time",
                   "cpu_time", "time_unit"}
        counters = {field: median(entry[field] for entry in entries)
                    for field, value in entries[0].items()
                    if field not in ignored and isinstance(value, (int, float))}
        if any(not math.isfinite(value) or value < 0 for entry in entries
               for field, value in entry.items()
               if field in counters):
            raise ValueError(f"{name}: invalid counter")
        result[name] = {"median_ns": median(times),
                        "variation_cv": stdev(times) / mean(times) if repetitions > 1 else 0,
                        "counters": counters}
    if any(modes != set(MODES) for modes in workloads.values()):
        raise ValueError("Escape comparison requires all three modes per workload")
    return result


def escape_comparison_report(report: dict, expected_cases: list[str], repetitions: int) -> str:
    results = aggregate_escape_results(report, expected_cases, repetitions)
    lines = ["逃逸优化：同一源码、LLVM O2、generic CPU、动态地址模型的封闭 AOT 机器码。",
             "普通堆 = 关闭逃逸优化；标量替换 = 字段拆分及按需物化；内部借用 = 再启用私有栈记录。",
             f"全部选定用例通过 {repetitions} 次重复及结果检查。执行计时排除编译、加载和实例化。",
             "分配量来自单独预热实例的一次执行，关闭自动 GC；计时实例使用普通 GC。",
             "没有运行时诊断计数插桩。帧字节是优化前声明存储，不是实测机器栈高水位。",
             "数值与对象图循环可能被 LLVM 整体归约，不能把这些比值外推到普通应用。", "",
             "| 负载 | 普通堆 ns | 标量替换 ns | 内部借用 ns | 内部借用/标量替换 | CV 堆/标量/借用 |",
             "| --- | ---: | ---: | ---: | ---: | --- |"]
    workloads = sorted({CASE.fullmatch(name)[2] for name in results if CASE.fullmatch(name)})
    for workload in workloads:
        values = [results[f"escape/{mode}/{workload}/real_time"] for mode in MODES]
        lines.append("| " + " | ".join([workload, *[f'{value["median_ns"]:.1f}' for value in values],
                                       f'{values[2]["median_ns"] / values[1]["median_ns"]:.3f}',
                                       "/".join(f'{value["variation_cv"]:.1%}' for value in values)]) + " |")
    fields = ("allocation_bytes", "allocated_objects", "field_allocation_bytes", "sample_collected_objects",
              "maximum_frame_bytes", "maximum_shadow_slots", "borrowed_frame_bytes", "borrowed_shadow_slots",
              "analysis_lowering_ns", "compile_ns",
              "executable_bytes", "read_only_bytes")
    lines.extend(["", "| 负载/配置 | " + " | ".join(fields) + " |",
                  "| --- | " + " | ".join("---:" for _ in fields) + " |"])
    for name, value in sorted(results.items()):
        if CASE.fullmatch(name):
            lines.append("| " + " | ".join([name.removeprefix("escape/"),
                *[f'{value["counters"][field]:.0f}' for field in fields]]) + " |")
    for name, value in sorted(results.items()):
        if RELOAD.fullmatch(name):
            counters = value["counters"]
            lines.append(f'\n{name}: 总耗时 {value["median_ns"] / 1e6:.3f} ms，'
                         f'提交暂停 {counters["pause_ns"] / 1000:.3f} us，'
                         f'物化 {counters["materialized_objects"]:.0f} 个对象，'
                         f'物化准备 {counters["materialization_ns"] / 1000:.3f} us，'
                         f'释放后代码批次 {counters["released_code_batches"]:.0f}。')
    return "\n".join(lines) + "\n"
