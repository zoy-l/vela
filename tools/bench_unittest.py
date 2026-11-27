"""Check backend completeness, timing phases, JIT evidence and command boundaries."""

from __future__ import annotations

import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

from tools.bench import parse_arguments, run
from tools.benchmark_report import aot_comparison_report, comparison_report, jit_comparison_report, native_call_report
from tools.escape_benchmark_report import aggregate_escape_results
from tools.gc_benchmark_report import gc_report
from tools.native_class_benchmark_report import aggregate_native_class_results
from tools.reflection_benchmark_report import aggregate_reflection_results, reflection_comparison_report


BACKENDS = ("cpp", "jit")


def reflection_samples(repetitions: int = 3) -> tuple[list[str], list[dict]]:
    names = [f"reflection/{backend}/field/{strategy}/real_time"
             for backend, strategies in (("baseline", ("direct", "dynamic", "cached")),
                                          ("cpp", ("cached",))) for strategy in strategies]
    query = "reflection/optimized/query/all/background:1024/matches:8/real_time"
    names += [query, "reflection/reload/resources/iterations:1/real_time"]
    rows = []
    for position, name in enumerate(names):
        operations = 8 if name == query else 16 if "/reload/" in name else 128
        result = 64 if name == query else 16 if "/reload/" in name else 8128
        for index in range(repetitions):
            rows.append(dict(name=name, real_time=operations * (position + 1) * (index + 1),
                             cpu_time=999999, time_unit="ns", operations=operations,
                             repetitions=repetitions, repetition_index=index,
                             result=result, expected_result=result, reflection_members=0,
                             allocation_bytes_per_op=80, baseline_bytes=100, allocated_bytes=200,
                             retained_bytes=100, committed_bytes=256, image_bytes=1024,
                             executable_bytes=256, reflection_types=1, retained_images=1,
                             background_members=1024, matches_per_query=8,
                             reflection_index_prepared=int(name == query or "/cached/" in name)))
    return names, rows


class ReflectionReportTest(unittest.TestCase):
    def test_uses_wall_time_and_batch_units_for_comparisons_and_queries(self) -> None:
        names, rows = reflection_samples()
        summary = aggregate_reflection_results({"benchmarks": rows}, names, 3)
        self.assertEqual(summary[names[2]]["ns_per_operation"], 6)
        self.assertEqual(summary[names[2]]["ratios"],
                         {"direct": 3, "dynamic": 1.5, "cpp_reference": 0.75})
        self.assertAlmostEqual(summary[names[2]]["wall_cv"], 0.5)
        self.assertEqual(summary[names[4]]["ns_per_operation"], 10)
        self.assertEqual(summary[names[4]]["counters"]["allocation_bytes_per_op"], 80)
        text = reflection_comparison_report({"benchmarks": rows}, names, 3)
        self.assertIn("| baseline/field/cached | 6.000 | 50.0% | 3.00x | 1.50x | 0.75x | 80 |", text)
        self.assertIn("| optimized / all | 1024 | 8 | 10.000 | 50.0% | 80 |", text)
        self.assertIn("contract differences", text)
        self.assertIn("no overhead is subtracted", text)
        self.assertIn("only one image may remain", text)

    def test_rejects_failed_incomplete_and_invalid_measurements(self) -> None:
        names, rows = reflection_samples()
        for field, value, diagnostic in (("result", 1, "checksum mismatch"),
                                         ("operations", 0, "operations"),
                                         ("allocation_bytes_per_op", float("nan"), "allocation"),
                                         ("reflection_index_prepared", 1, "reflection index"),
                                         ("reflection_members", 1, "unregistered")):
            invalid = [dict(row) for row in rows]
            invalid[0][field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, diagnostic):
                aggregate_reflection_results({"benchmarks": invalid}, names, 3)
        for field, value, diagnostic in (("background_members", 64, "cardinality"),
                                         ("matches_per_query", 1, "cardinality")):
            invalid = [dict(row) for row in rows]
            invalid[-4][field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, diagnostic):
                aggregate_reflection_results({"benchmarks": invalid}, names, 3)
        rows[-1]["retained_images"] = 2
        with self.assertRaisesRegex(ValueError, "retired code image"):
            aggregate_reflection_results({"benchmarks": rows}, names, 3)
        with self.assertRaisesRegex(ValueError, "incomplete"):
            aggregate_reflection_results({"benchmarks": rows[:-1]}, names, 3)

    def test_registered_unused_control_keeps_index_lazy(self) -> None:
        names, rows = reflection_samples()
        unused = "reflection/baseline/unused/registered_direct/real_time"
        selected = rows[:3] + [dict(row, name=unused, reflection_members=3,
                                  reflection_index_prepared=0) for row in rows[6:9]]
        summary = aggregate_reflection_results({"benchmarks": selected}, [names[0], unused], 3)
        self.assertEqual(summary[unused]["ratios"]["direct"], 3)
        self.assertEqual(summary[unused]["counters"]["reflection_index_prepared"], 0)
        selected[-1]["reflection_index_prepared"] = 1
        with self.assertRaisesRegex(ValueError, "reflection index"):
            aggregate_reflection_results({"benchmarks": selected}, [names[0], unused], 3)

    def test_aot_pairs_operations_and_queries_with_offline_object_sizes(self) -> None:
        names, rows = reflection_samples()
        selected = []
        for row in rows:
            name = row["name"]
            if "/baseline/" in name or "/query/" in name:
                item = dict(row, name=name.replace("/baseline/", "/aot/").replace("/optimized/", "/aot/"),
                            aot_object_bytes=4096)
                del item["image_bytes"]
                del item["executable_bytes"]
                selected.append(item)
            elif "/cpp/" in name:
                selected.append(row)
        expected = list(dict.fromkeys(row["name"] for row in selected))
        summary = aggregate_reflection_results({"benchmarks": selected}, expected, 3)
        cached = names[2].replace("/baseline/", "/aot/")
        self.assertEqual(summary[cached]["ns_per_operation"], 6)
        self.assertEqual(summary[cached]["ratios"]["cpp_reference"], 0.75)
        self.assertEqual(summary[cached]["counters"]["aot_object_bytes"], 4096)
        self.assertNotIn("executable_bytes", summary[cached]["counters"])
        text = reflection_comparison_report({"benchmarks": selected}, expected, 3)
        self.assertIn("| aot/field/cached | 6.000", text)
        self.assertIn("| aot / all | 1024 | 8 | 10.000", text)
        self.assertIn("| field/cached | 4096 |", text)
        self.assertIn("vela build --emit=obj", text)
        for size in (0, float("nan")):
            invalid = [dict(row) for row in selected]
            invalid[0]["aot_object_bytes"] = size
            with self.subTest(size=size), self.assertRaisesRegex(ValueError, "AOT object|aot_object"):
                aggregate_reflection_results({"benchmarks": invalid}, expected, 3)


class NativeClassReportTest(unittest.TestCase):
    def rows(self) -> tuple[list[str], list[dict]]:
        names = [f"native_class/aot/{path}/integer/real_time" for path in ("class", "free")]
        rows = [{"name": name, "real_time": 100 + index, "time_unit": "ns",
                 "repetitions": 3, "repetition_index": index, "calls_per_iteration": 10,
                 "allocations_per_iteration": 0, "shared_owner_delta": 0}
                for name in names for index in range(3)]
        return names, rows

    def test_pairs_medians_and_variation_only_after_complete_correct_samples(self) -> None:
        names, rows = self.rows()
        summary = aggregate_native_class_results({"benchmarks": rows}, names, 3)
        self.assertAlmostEqual(summary[names[0]]["ns_per_call"], 10.1)
        self.assertEqual(summary[names[0]]["class_to_free"], 1)
        self.assertGreater(summary[names[0]]["combined_stddev_ns"], 0)
        for field, value, diagnostic in (("error_occurred", True, "benchmark failed"),
                                         ("allocations_per_iteration", 1, "allocated"),
                                         ("shared_owner_delta", 1, "shared owner")):
            invalid = [dict(row) for row in rows]
            invalid[-1][field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, diagnostic):
                aggregate_native_class_results({"benchmarks": invalid}, names, 3)
        with self.assertRaisesRegex(ValueError, "incomplete"):
            aggregate_native_class_results({"benchmarks": rows[:-1]}, names, 3)
        repeated_names = [name.replace("integer", "repeated") for name in names]
        repeated = [dict(row, name=row["name"].replace("integer", "repeated")) for row in rows]
        repeated[-1]["allocations_per_iteration"] = 1
        with self.assertRaisesRegex(ValueError, "allocated"):
            aggregate_native_class_results({"benchmarks": repeated}, repeated_names, 3)


def case(backend: str, phase: str = "compare") -> str:
    return f"{phase}/{backend}/sieve/10000/real_time"


def sample(backend: str, elapsed: float, unit: str = "ns",
           phase: str = "compare", repetition: int = 0, repetitions: int = 1) -> dict:
    return {
        "name": case(backend, phase), "real_time": elapsed, "cpu_time": 999999999,
        "time_unit": unit, "repetitions": repetitions, "repetition_index": repetition,
        "baseline_bytes": 100, "allocated_bytes": 150, "retained_bytes": 100,
        "committed_bytes": 200,
    }


def steady_samples(repetitions: int = 1) -> list[dict]:
    return [sample(backend, (index + 1) * 1000, repetition=repetition, repetitions=repetitions)
            for repetition in range(repetitions) for index, backend in enumerate(BACKENDS)]


def native_samples(repetitions: int = 1) -> list[dict]:
    return [dict(sample("cpp", elapsed, repetition=index, repetitions=repetitions),
                 name=f"native/{backend}/integer/64/real_time", calls_per_iteration=64,
                 baseline_heap_bytes=100, end_heap_bytes=100,
                 baseline_heap_objects=2, end_heap_objects=2)
            for backend, elapsed in (("cpp", 128), ("cpp_indirect", 192),
                                     ("baseline_control", 640), ("baseline", 1920),
                                     ("optimized_control", 384), ("optimized", 1536),
                                     ("aot_control", 320), ("aot", 1280))
            for index in range(repetitions)]


def ported_aot_samples(repetitions: int = 1) -> list[dict]:
    return [{"name": f"aot/{backend}/queen/8/real_time", "real_time": elapsed,
             "time_unit": "us", "result": 92, "expected_result": 92,
             "repetition_index": index, "repetitions": repetitions,
             "process_resident_bytes": 4096, "baseline_bytes": 100, "retained_bytes": 100,
             "allocated_bytes": 200, "committed_bytes": 256, "suspended_frames": 0,
             "saved_state_bytes": 0, "remaining_suspended_frames": 0, "remaining_saved_bytes": 0,
             "entry_frame_bytes": 128, "entry_shadow_slots": 1, "entry_static_root_stores": 2}
            for backend, elapsed in (("native", 20), ("cpp", 10))
            for index in range(repetitions)]


def reloadable_samples(repetitions: int = 3) -> dict:
    rows = ported_aot_samples(repetitions)
    template = rows[0]
    for backend, elapsed in (("baseline", 40), ("optimized", 30)):
        rows += [dict(template, name=f"hot/steady/{backend}/queen/8/real_time",
                      repetition_index=index, real_time=elapsed,
                      code_batches=1, code_executable_bytes=1024, code_read_only_bytes=0,
                      code_writable_bytes=0, external_field_bytes=128,
                      **dict.fromkeys(("load_ns", "source_total_ns", "code_total_ns",
                                       "file_capture_ns", "engine_creation_ns", "source_loading_ns",
                                       "type_checking_ns", "typed_ir_ns", "suite_preparation_ns",
                                       "llvm_lowering_ns", "llvm_optimization_ns", "machine_compilation_ns",
                                       "linking_ns", "instantiation_ns", "initial_publication_pause_ns",
                                       "first_execution_ns"), 1000)) for index in range(repetitions)]
    return {"benchmarks": rows, "context": {"jit_steady_setup": "shared-image/fresh-isolate/v1",
                                            "jit_prepared_suites": "2"}}


def gc_samples(repetitions: int = 3) -> list[dict]:
    rows = []
    for old_objects, survivors in ((0, 64), (8192, 4096)):
        for index in range(repetitions):
            live_bytes = 1024 + survivors * 64
            reclaimed_bytes = (4096 - survivors) * 64
            rows.append(dict(
                name=f"gc/cohort/{old_objects}/{survivors}/16/manual_time",
                real_time=(index + 1) * 1000, cpu_time=999999, time_unit="ns",
                repetitions=repetitions, repetition_index=index, old_objects=old_objects,
                new_objects=4096, new_survivors=survivors, start_ns=50, p95_step_ns=100,
                maximum_step_ns=(index + 1) * 200, steps_per_cycle=257,
                heap_before_bytes=live_bytes + reclaimed_bytes, heap_live_bytes=live_bytes,
                heap_committed_bytes=live_bytes + reclaimed_bytes,
                reclaimed_bytes=reclaimed_bytes, reclaimed_objects=4096 - survivors))
    return rows


class BenchmarkReportTest(unittest.TestCase):
    def test_workload_manifests_have_unique_unqualified_names(self) -> None:
        root = Path(__file__).resolve().parent.parent / "benchmarks"
        basic = json.loads((root / "aot_cases.json").read_text(encoding="utf-8"))
        algorithms = json.loads((root / "algorithms/cases.json").read_text(encoding="utf-8"))
        cases = basic + algorithms
        keys = [(case["name"], case["size"]) for case in cases]
        self.assertEqual(len(keys), 29)
        self.assertEqual(len(keys), len(set(keys)))
        basic_names = {case["name"] for case in basic}
        algorithm_names = {case["name"] for case in algorithms}
        self.assertFalse(basic_names & algorithm_names)
        self.assertIn("spectralNormInt", basic_names)
        self.assertIn("temporaryObjects", basic_names)
        self.assertNotIn("gc", basic_names)
        self.assertIn("spectralNorm", algorithm_names)
        numeric = [case for case in basic if case["name"] == "arithmetic"]
        self.assertEqual([case["size"] for case in numeric], [1000, 4096, 65536])
        for case in numeric:
            seed = 42
            expected = 0
            for _ in range(case["size"]):
                seed = (seed * 1664525 + 1013904223) & 0x7fffffff
                expected += (seed & 1023) ** 2
            self.assertEqual(case["expected"], expected)

    def test_temporary_object_report_exposes_eliminated_allocations_in_both_comparisons(self) -> None:
        report = reloadable_samples()
        rows = report["benchmarks"]
        for row in rows:
            row["name"] = row["name"].replace("/queen/8/", "/temporaryObjects/1000/")
            count = 1000 if "/optimized/" in row["name"] else 0
            row.update(allocations_per_invocation=count, allocation_bytes_per_invocation=count * 64)
        names = sorted({row["name"] for row in rows})
        text = jit_comparison_report(report, names, 3)
        self.assertIn("| temporaryObjects | 1000 | native | 0.0 | 0.0 |", text)
        self.assertIn("| temporaryObjects | 1000 | optimized | 1000.0 | 64000.0 |", text)
        self.assertIn("not collector-speed comparisons", text)
        self.assertIn("final forced collection is excluded", text)
        aot_rows = [row for row in rows if row["name"].startswith("aot/")]
        aot_names = sorted({row["name"] for row in aot_rows})
        self.assertIn("Managed allocations/invocation",
                      aot_comparison_report({"benchmarks": aot_rows}, aot_names, 3))
        rows[-1]["allocations_per_invocation"] = -1
        with self.assertRaisesRegex(ValueError, "invalid allocations_per_invocation"):
            jit_comparison_report(report, names, 3)

    def test_gc_report_separates_cycle_time_steps_and_reclaimed_memory(self) -> None:
        rows = gc_samples()
        names = sorted({row["name"] for row in rows})
        text = gc_report({"benchmarks": rows}, names, 3)
        self.assertIn("| 0 | 4096 | 64 | 16 | 2.000 | 50.0% | 50.0 | 100.0 | 600.0 | 257.0 |", text)
        self.assertIn("| 8192 | 4096 | 16 | 263168 | 263168 | 263168 | 0 | 0 |", text)
        self.assertIn("Per-step clock reads", text)
        self.assertIn("no C++ reference", text)
        self.assertNotIn("/C++", text)

    def test_gc_report_rejects_missing_failed_and_inconsistent_samples(self) -> None:
        rows = gc_samples()
        names = sorted({row["name"] for row in rows})
        with self.assertRaisesRegex(ValueError, "incomplete"):
            gc_report({"benchmarks": rows[:-1]}, names, 3)
        for field, value, message in (("error_occurred", True, "benchmark failed"),
                                       ("start_ns", float("nan"), "invalid start_ns"),
                                       ("new_objects", 4096.5, "incorrect cohort counts"),
                                       ("new_objects", 1, "incorrect cohort counts"),
                                       ("reclaimed_objects", 1, "incorrect cohort counts"),
                                       ("reclaimed_bytes", 1, "inconsistent heap accounting"),
                                       ("heap_committed_bytes", 0, "inconsistent heap accounting"),
                                       ("steps_per_cycle", 0, "invalid step measurements"),
                                       ("maximum_step_ns", 0, "invalid step measurements")):
            with self.subTest(field=field):
                invalid = [dict(row) for row in rows]
                invalid[-1][field] = value
                with self.assertRaisesRegex(ValueError, message):
                    gc_report({"benchmarks": invalid}, names, 3)
        invalid_name = "aot/native/temporaryObjects/1000/real_time"
        invalid = [dict(row, name=invalid_name) for row in rows[:3]]
        with self.assertRaisesRegex(ValueError, "not a collector cycle"):
            gc_report({"benchmarks": invalid}, [invalid_name], 3)
        invalid = [dict(row, name=row["name"].replace("/16/", "/0/")) for row in rows]
        with self.assertRaisesRegex(ValueError, "invalid step budget"):
            gc_report({"benchmarks": invalid}, sorted({row["name"] for row in invalid}), 3)
        fractional = [dict(row, new_objects=4096.5,
                           reclaimed_objects=4096.5 - row["new_survivors"]) for row in rows]
        with self.assertRaisesRegex(ValueError, "incorrect cohort counts"):
            gc_report({"benchmarks": fractional}, names, 3)

    def test_unqualified_workloads_keep_distinct_preparation_suites(self) -> None:
        report = reloadable_samples(1)
        basic = [dict(row, name=row["name"].replace("/queen/8/", "/spectralNormInt/500/"))
                 for row in report["benchmarks"]]
        for row in basic:
            if row["name"].startswith("hot/"):
                row["suite_preparation_ns"] = 2000
        report["benchmarks"].extend(basic)
        report["context"]["jit_prepared_suites"] = "4"
        names = [row["name"] for row in report["benchmarks"]]
        text = jit_comparison_report(report, names, 1)
        for suite in ("aot", "algorithms"):
            for backend in ("baseline", "optimized"):
                self.assertEqual(text.count(f"| {suite} | {backend} |"), 1)

    def test_escape_report_requires_complete_modes_checksums_and_virtual_code_retirement(self) -> None:
        rows = [dict(name=f"escape/{mode}/numeric/real_time", real_time=time, time_unit="ns",
                     result=42, expected_result=42, allocation_bytes=128)
                for mode, time in (("heap", 100), ("scalar", 60), ("borrow", 50))]
        names = [row["name"] for row in rows]
        values = aggregate_escape_results({"benchmarks": rows}, names, 1)
        self.assertEqual(values[names[-1]]["median_ns"], 50)
        with self.assertRaisesRegex(ValueError, "all three modes"):
            aggregate_escape_results({"benchmarks": rows[:-1]}, names[:-1], 1)
        rows[-1]["result"] = 0
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            aggregate_escape_results({"benchmarks": rows}, names, 1)
        rows[-1]["result"] = 42
        rows[-1]["allocation_bytes"] = -1
        with self.assertRaisesRegex(ValueError, "invalid counter"):
            aggregate_escape_results({"benchmarks": rows}, names, 1)
        rows[-1]["allocation_bytes"] = 0
        lifecycle = dict(name="hot/optimized/virtual/manual_time", real_time=10, time_unit="us",
                         released_code_batches=3, released_code_images=2, released_saved_state_bytes=0,
                         released_suspended_frames=0, materialized_objects=1)
        rows.append(lifecycle)
        names.append(lifecycle["name"])
        with self.assertRaisesRegex(ValueError, "code retirement"):
            aggregate_escape_results({"benchmarks": rows}, names, 1)
        lifecycle["released_code_images"] = 1
        self.assertEqual(len(aggregate_escape_results({"benchmarks": rows}, names, 1)), 4)

    def test_reloadable_report_requires_four_checked_backends_and_code_retirement_evidence(self) -> None:
        report = reloadable_samples()
        rows = report["benchmarks"]
        names = sorted({row["name"] for row in rows})
        text = jit_comparison_report(report, names, 3)
        self.assertIn("| queen | 8 | 10.000 | 20.000 | 40.000 | 30.000 | 2.00x | 1.50x | 2.00x | 3.00x |", text)
        self.assertIn("AOT ≤ 2× C++: 1/1; optimized JIT ≤ 3× C++: 1/1", text)
        self.assertIn("Geometric mean optimized JIT/AOT over 1 paired workloads: 1.50x.", text)
        for field, invalid in (("code_read_only_bytes", -1), ("code_read_only_bytes", float("nan")),
                               ("code_executable_bytes", 0), ("code_batches", 0)):
            with self.subTest(field=field, invalid=invalid):
                original = rows[-1][field]
                rows[-1][field] = invalid
                with self.assertRaisesRegex(ValueError, "invalid " + field):
                    jit_comparison_report(report, names, 3)
                rows[-1][field] = original
        missing = [row for row in rows if "/optimized/" not in row["name"]]
        with self.assertRaisesRegex(ValueError, "requires matching"):
            jit_comparison_report({"benchmarks": missing}, sorted({row["name"] for row in missing}), 3)
        rows[-1]["remaining_suspended_frames"] = 1
        with self.assertRaisesRegex(ValueError, "retained temporary state"):
            jit_comparison_report(report, names, 3)

    def test_reloadable_report_separates_shared_preparation_and_rejects_recompilation(self) -> None:
        report = reloadable_samples()
        rows = report["benchmarks"]
        names = sorted({row["name"] for row in rows})
        text = jit_comparison_report(report, names, 3)
        self.assertIn("single setup observations", text)
        self.assertIn("Fresh-Isolate setup", text)
        self.assertEqual(text.count("| algorithms | optimized |"), 1)
        rows[-1]["suite_preparation_ns"] += 1
        with self.assertRaisesRegex(ValueError, "compiled more than once"):
            jit_comparison_report(report, names, 3)
        rows[-1]["suite_preparation_ns"] -= 1
        report["context"]["jit_prepared_suites"] = "3"
        with self.assertRaisesRegex(ValueError, "preparation context"):
            jit_comparison_report(report, names, 3)

    def test_reload_lifecycle_report_rejects_incomplete_retirement_and_invalid_pause_measurements(self) -> None:
        name = "hot/optimized/active/manual_time"
        row = dict.fromkeys(("pause_ns", "pause_max_ns", "source_ns", "llvm_lowering_ns",
                             "llvm_optimization_ns", "machine_compilation_ns", "linking_ns",
                             "instantiation_ns", "validation_ns", "state_preparation_ns",
                             "deoptimization_ns", "commit_ns", "refresh_ns", "entry_ns",
                             "stopping_ns", "new_isolate_preparation_ns", "safepoint_waiting_ns",
                             "materialization_ns"), 1000)
        row.update(name=name, real_time=20, time_unit="us", released_code_batches=3, released_code_images=1,
                   released_heap_bytes=128, released_external_field_bytes=0,
                   released_executable_bytes=1024, released_read_only_bytes=2048,
                   released_constant_payload_bytes=32, released_debug_payload_bytes=1024,
                   released_writable_bytes=0, released_image_data_bytes=1024,
                   released_suspended_frames=0, released_saved_state_bytes=0,
                   materialized_objects=0)
        report = {"benchmarks": [row]}
        text = jit_comparison_report(report, [name], 1)
        self.assertIn("20.000", text)
        self.assertIn("pause_max_ns", text)
        self.assertNotIn("Geometric mean", text)
        row["released_code_images"] = 2
        with self.assertRaisesRegex(ValueError, "survived release"):
            jit_comparison_report(report, [name], 1)
        row["released_code_images"] = 1
        row["pause_ns"] = float("nan")
        with self.assertRaisesRegex(ValueError, "invalid pause_ns"):
            jit_comparison_report(report, [name], 1)
        row["pause_ns"] = 1000
        name = "hot/optimized/virtual/manual_time"
        row.update(name=name, materialized_objects=1)
        text = jit_comparison_report(report, [name], 1)
        self.assertIn("Materialized objects", text)
        self.assertIn("materialization us", text)
        row["materialized_objects"] = 0
        with self.assertRaisesRegex(ValueError, "incorrect materialized object count"):
            jit_comparison_report(report, [name], 1)

    def test_reload_scaling_report_checks_affected_functions_and_image_retirement(self) -> None:
        rows = []
        for tier in ("baseline", "optimized"):
            for graph, files in (("independent", 256), ("imports", 256), ("fanout", 32)):
                affected = 1 if tier == "baseline" or graph == "independent" else (
                    files if graph == "fanout" else 2)
                rows.append(dict(
                    name=f"hot/scaling/{tier}/{graph}/{files}/manual_time", real_time=20,
                    time_unit="us", files=files, affected_functions=affected,
                    compiled_functions=affected, released_code_images=1,
                    **dict.fromkeys(("source_ns", "source_loading_ns", "type_checking_ns", "typed_ir_ns",
                                     "code_ns", "pause_ns", "llvm_lowering_ns",
                                     "llvm_optimization_ns", "machine_compilation_ns", "linking_ns"), 1000)))
        names = [row["name"] for row in rows]
        report = {"benchmarks": rows}
        text = jit_comparison_report(report, names, 1)
        self.assertIn("Hot reload scaling", text)
        self.assertIn("fanout/32/manual_time | 20.000", text)
        self.assertNotIn("Geometric mean", text)
        rows[1]["compiled_functions"] = 256
        with self.assertRaisesRegex(ValueError, "invalid incremental compilation"):
            jit_comparison_report(report, names, 1)
        rows[1]["compiled_functions"] = 1
        rows[1]["released_code_images"] = 2
        with self.assertRaisesRegex(ValueError, "image retirement"):
            jit_comparison_report(report, names, 1)
        rows[1]["released_code_images"] = 1
        rows[1]["source_ns"] = float("inf")
        with self.assertRaisesRegex(ValueError, "invalid source_ns"):
            jit_comparison_report(report, names, 1)

    def test_aot_report_pairs_backends_and_checks_results_and_retained_state(self) -> None:
        rows = ported_aot_samples(3)
        names = sorted({row["name"] for row in rows})
        report = {"benchmarks": rows}
        text = aot_comparison_report(report, names, 3)
        self.assertIn("| queen | 8 | 20.000 | 10.000 | - | 2.000 |", text)
        self.assertIn("over 1 paired workloads: 2.00x", text)
        self.assertIn("| queen | 8 | 128 | 1 | 2 |", text)
        self.assertIn("| queen | 8 | cpp | 4096 |", text)
        self.assertIn("not measured machine-stack usage", text)
        rows[0]["remaining_suspended_frames"] = 1
        with self.assertRaisesRegex(ValueError, "retained temporary state"):
            aot_comparison_report(report, names, 3)
        rows[0]["remaining_suspended_frames"] = 0
        rows[-1]["result"] = 0
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            aot_comparison_report(report, names, 3)
        for row in rows:
            del row["expected_result"]
        with self.assertRaisesRegex(ValueError, "checksums differ"):
            aot_comparison_report(report, names, 3)

    def test_aot_report_checks_floating_results_with_the_declared_tolerance(self) -> None:
        rows = ported_aot_samples()
        names = [row["name"] for row in rows]
        for index, row in enumerate(rows):
            row["expected_result"] = 1.274224115941234
            row["result"] = row["expected_result"] + index * 1e-15
        self.assertIn("queen", aot_comparison_report({"benchmarks": rows}, names, 1))
        rows[-1]["result"] += 1e-6
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            aot_comparison_report({"benchmarks": rows}, names, 1)

    def test_uses_wall_time_medians_and_separates_cold_compilation_and_memory(self) -> None:
        rows = []
        for backend, values in zip(BACKENDS, ((1, 3, 2), (8, 10, 12))):
            rows.extend(sample(backend, value, "us", repetition=index, repetitions=3)
                        for index, value in enumerate(values))
        for phase, backends in (("first", ("jit",)), ("compile", ("representative",))):
            rows.extend(sample(backend, 3, "ms", phase, repetition=index, repetitions=3)
                        for backend in backends for index in range(3))
        aggregate = sample("cpp", 999999)
        aggregate.update(run_type="aggregate", aggregate_name="mean")
        names = list(dict.fromkeys(row["name"] for row in rows))
        result = comparison_report({"benchmarks": [*rows, aggregate]}, names, 3)
        self.assertIn("| sieve | 10000 | 2.000 | 10.000 | 5.00x |", result)
        self.assertIn("3000.000", result)
        self.assertIn("| sieve | 10000 | Vela JIT | 100 | 150 | 100 | 200 |", result)
        self.assertIn("First invocation in a fresh state", result)
        self.assertIn("Compilation with a reusable JIT engine", result)

    def test_converts_units_without_using_cpu_time(self) -> None:
        rows = [sample("cpp", 1000), sample("jit", 0.000010, "s")]
        result = comparison_report({"benchmarks": rows}, [case(backend) for backend in BACKENDS], 1)
        self.assertIn("| 1.000 | 10.000 | 10.00x |", result)

    def test_rejects_missing_backends_workloads_and_duplicate_or_missing_repetitions(self) -> None:
        names = [case(backend) for backend in BACKENDS]
        with self.assertRaisesRegex(ValueError, "missing"):
            comparison_report({"benchmarks": steady_samples()[:-1]}, names, 1)
        with self.assertRaisesRegex(ValueError, "missing"):
            comparison_report({"benchmarks": steady_samples()}, names + [case("cpp").replace("sieve", "particles")], 1)
        rows = steady_samples(3)
        with self.assertRaisesRegex(ValueError, "repetitions"):
            comparison_report({"benchmarks": rows[:-1]}, names, 3)
        rows[-1]["repetition_index"] = 0
        with self.assertRaisesRegex(ValueError, "repetitions"):
            comparison_report({"benchmarks": rows}, names, 3)

    def test_rejects_failed_nonfinite_or_invalid_heap_reports(self) -> None:
        names = [case(backend) for backend in BACKENDS]
        rows = steady_samples()
        rows[1].update(error_occurred=True, error_message="checksum mismatch")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            comparison_report({"benchmarks": rows}, names, 1)
        rows = steady_samples()
        rows[0]["real_time"] = float("nan")
        with self.assertRaisesRegex(ValueError, "invalid wall time"):
            comparison_report({"benchmarks": rows}, names, 1)
        rows = steady_samples()
        rows[-1]["allocated_bytes"] = -1
        with self.assertRaisesRegex(ValueError, "invalid allocated_bytes"):
            comparison_report({"benchmarks": rows}, names, 1)
        with self.assertRaisesRegex(ValueError, "No supported"):
            comparison_report({"benchmarks": []}, [], 1)

    def test_explicit_backend_filter_reports_only_selected_backends(self) -> None:
        rows = steady_samples()[1:]
        result = comparison_report({"benchmarks": rows}, [row["name"] for row in rows], 1)
        self.assertIn("| sieve | 10000 | - | 2.000 | - |", result)

    def test_documents_api_baseline_differences_only_for_selected_workloads(self) -> None:
        entries = [sample("cpp", 1000), sample("jit", 5000)]
        for entry in entries:
            entry["name"] = entry["name"].replace("sieve", "json_roundtrip")
        names = [entry["name"] for entry in entries]
        result = comparison_report({"benchmarks": entries}, names, 1)
        self.assertIn("pinned yyjson DOM arenas", result)
        self.assertIn("representation and contract differences", result)
        self.assertNotIn("ASCII character mapping", result)
        ordinary_entries = [sample("cpp", 1000), sample("jit", 5000)]
        ordinary = comparison_report({"benchmarks": ordinary_entries},
                                     [entry["name"] for entry in ordinary_entries], 1)
        self.assertNotIn("Native baseline contracts", ordinary)

    def test_reports_api_allocation_pressure_without_treating_it_as_cpp_memory(self) -> None:
        rows = [sample("cpp", 1000), sample("jit", 5000)]
        for entry in rows:
            entry["name"] = entry["name"].replace("sieve", "string_replace_bytes")
        rows[1].update(allocation_bytes_per_iteration=4096,
                       allocation_count_per_iteration=256, peak_live_growth_bytes=8192)
        names = [entry["name"] for entry in rows]
        report = comparison_report({"benchmarks": rows}, names, 1)
        self.assertIn("| string_replace_bytes | 10000 | 4096.00 | 256.00 | 8192.00 |", report)
        self.assertIn("not C++ allocator measurements", report)
        self.assertIn("reserves one result", report)
        rows[1].pop("allocation_count_per_iteration")
        with self.assertRaises(ValueError):
            comparison_report({"benchmarks": rows}, names, 1)

    def test_comparison_options_preserve_google_benchmark_arguments(self) -> None:
        parsed = parse_arguments(["--compare", "--report", "out/test.json", "--",
                                  "--benchmark_repetitions=3"])
        self.assertEqual(parsed.report_mode, "compare")
        self.assertEqual(parsed.variant, "release")
        self.assertEqual(parsed.benchmark_arguments, ["--benchmark_repetitions=3"])

    def test_native_report_normalizes_batches_and_labels_subtraction_and_heap_limits(self) -> None:
        rows = native_samples(3)
        result = native_call_report({"benchmarks": rows}, sorted({row["name"] for row in rows}), 3)
        self.assertIn("| integer | 64 | 2.000 | 3.000 | 30.000 | 24.000 | 20.000 | 15.000 | 12.000 | 10.000 | 3/3/3/3/3/3/3/3 |", result)
        self.assertIn("| integer | 64 | 10.000 | 6.000 | 5.000 | 20.000 | 18.000 | 15.000 |", result)
        self.assertIn("not paired samples", result)
        self.assertIn("not cumulative allocation counts", result)
        self.assertIn("| integer | 64 | baseline | 100 | 100 | 2 | 2 |", result)
        self.assertIn("| integer | 64 | optimized | 100 | 100 | 2 | 2 |", result)
        self.assertIn("| integer | 64 | aot | 100 | 100 | 2 | 2 |", result)

    def test_native_report_rejects_incomplete_samples_and_bad_counters(self) -> None:
        rows = native_samples()
        names = [row["name"] for row in rows]
        with self.assertRaisesRegex(ValueError, "missing"):
            native_call_report({"benchmarks": rows[:-1]}, names, 1)
        rows[-1]["calls_per_iteration"] = 1
        with self.assertRaisesRegex(ValueError, "invalid call count"):
            native_call_report({"benchmarks": rows}, names, 1)
        rows = native_samples()
        rows[-1]["end_heap_bytes"] = float("nan")
        with self.assertRaisesRegex(ValueError, "invalid end_heap_bytes"):
            native_call_report({"benchmarks": rows}, names, 1)
    def test_native_report_requires_every_backend(self) -> None:
        for backend in ("cpp", "cpp_indirect", "baseline", "baseline_control",
                        "optimized", "optimized_control", "aot", "aot_control"):
            with self.subTest(backend=backend):
                rows = [row for row in native_samples() if f"/{backend}/" not in row["name"]]
                with self.assertRaisesRegex(ValueError, "all eight backends"):
                    native_call_report({"benchmarks": rows}, [row["name"] for row in rows], 1)

    def test_report_modes_use_their_registry_and_write_the_matching_report(self) -> None:
        cases = (
            ("--reflection-compare", "vela_benchmarks", "^reflection/",
             {"benchmarks": reflection_samples(7)[1]}, "Runtime reflection (Release)"),
            ("--native-compare", "vela_aot_benchmarks", "^native/",
             {"benchmarks": native_samples(7)}, "Baseline JIT native ns"),
            ("--jit-compare", "vela_aot_benchmarks", "^(aot/(native|cpp)/|hot/steady/)",
             reloadable_samples(7), "Fresh-Isolate setup"),
            ("--gc-report", "vela_benchmarks", "^gc/cohort/",
             {"benchmarks": gc_samples(7)}, "Vela GC: complete incremental collection cycles"),
        )
        for mode, executable, pattern, samples, heading in cases:
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary)
                report = output / "results.json"
                parsed = parse_arguments([mode, "--report", str(report)])
                names = sorted({row["name"] for row in samples["benchmarks"]})

                def execute(command: list[str], **kwargs: object) -> subprocess.CompletedProcess:
                    binary = Path(command[0])
                    self.assertEqual((binary.parent, binary.stem), (output, executable))
                    self.assertIn(f"--benchmark_filter={pattern}", command)
                    if "--benchmark_list_tests=true" in command:
                        return subprocess.CompletedProcess(command, 0, stdout="\n".join(names))
                    self.assertIn("--benchmark_repetitions=7", command)
                    self.assertIn("--benchmark_min_time=0.2s", command)
                    self.assertIn("--benchmark_enable_random_interleaving=true", command)
                    self.assertIn(f"--benchmark_out={report}", command)
                    self.assertIn("--benchmark_out_format=json", command)
                    report.write_text(json.dumps(samples), encoding="utf-8")
                    return subprocess.CompletedProcess(command, 0)

                with patch("tools.bench.build", return_value=output), \
                        patch("tools.bench.subprocess.run", side_effect=execute):
                    with redirect_stdout(io.StringIO()):
                        run(parsed)
                self.assertIn(heading, report.with_suffix(".md").read_text(encoding="utf-8"))

    def test_runner_rejects_benchmark_error_even_when_process_succeeds(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vela-bench-") as temporary:
            report = Path(temporary) / "results.json"
            markdown = report.with_suffix(".md")
            report.write_text("stale JSON", encoding="utf-8")
            markdown.write_text("stale success", encoding="utf-8")
            parsed = parse_arguments(["--compare", "--report", str(report)])

            def execute(command: list[str], **ignored_kwargs: object) -> subprocess.CompletedProcess:
                self.assertFalse(report.exists())
                self.assertFalse(markdown.exists())
                if "--benchmark_list_tests=true" in command:
                    return subprocess.CompletedProcess(command, 0, stdout="\n".join(case(backend) for backend in BACKENDS))
                self.assertIn("--benchmark_enable_random_interleaving=true", command)
                rows = steady_samples(7)
                rows[-1].update(error_occurred=True, error_message="stack overflow")
                report.write_text(json.dumps({"benchmarks": rows}), encoding="utf-8")
                return subprocess.CompletedProcess(command, 0)

            with patch("tools.bench.build", return_value=report.parent), \
                    patch("tools.bench.subprocess.run", side_effect=execute):
                with self.assertRaisesRegex(ValueError, "stack overflow"):
                    run(parsed)
            self.assertFalse(markdown.exists())

    def test_runner_keeps_user_repetitions_and_uses_the_filtered_binary_registry(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vela-bench-") as temporary:
            report = Path(temporary) / "results.json"
            parsed = parse_arguments(["--compare", "--report", str(report), "--",
                                      "--benchmark_repetitions=3", "--benchmark_filter=^compare/"])

            def execute(command: list[str], **ignored_kwargs: object) -> subprocess.CompletedProcess:
                self.assertIn("--benchmark_filter=^compare/", command)
                if "--benchmark_list_tests=true" in command:
                    return subprocess.CompletedProcess(command, 0, stdout="\n".join(case(backend) for backend in BACKENDS))
                self.assertIn("--benchmark_repetitions=3", command)
                self.assertNotIn("--benchmark_repetitions=7", command)
                report.write_text(json.dumps({"benchmarks": steady_samples(3)}), encoding="utf-8")
                return subprocess.CompletedProcess(command, 0)

            with patch("tools.bench.build", return_value=report.parent), \
                    patch("tools.bench.subprocess.run", side_effect=execute):
                with redirect_stdout(io.StringIO()):
                    run(parsed)
            self.assertIn("Vela/C++", report.with_suffix(".md").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
