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
from tools.benchmark_report import comparison_report, native_call_report


BACKENDS = ("cpp", "vm", "luajit_interp", "luajit")


def case(backend: str, phase: str = "compare") -> str:
    return f"{phase}/{backend}/sieve/10000/real_time"


def sample(backend: str, elapsed: float, unit: str = "ns",
           phase: str = "compare", repetition: int = 0, repetitions: int = 1) -> dict:
    traces = 10 if backend == "luajit" else 0
    return {
        "name": case(backend, phase), "real_time": elapsed, "cpu_time": 999999999,
        "time_unit": unit, "repetitions": repetitions, "repetition_index": repetition,
        "baseline_bytes": 100, "allocated_bytes": 150, "retained_bytes": 100,
        "committed_bytes": 200, "register_bytes": 80, "warmup_calls": 128,
        "warm_traces": traces, "traces": traces,
        "warm_jit_code_bytes": traces * 20, "jit_code_bytes": traces * 20,
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
                                     ("vm_control", 640), ("vm", 1920))
            for index in range(repetitions)]


class BenchmarkReportTest(unittest.TestCase):
    def test_uses_wall_time_medians_and_separates_cold_compilation_and_memory(self) -> None:
        rows = []
        for backend, values in zip(BACKENDS, ((1, 3, 2), (8, 10, 12), (4, 5, 6), (1, 2, 3))):
            rows.extend(sample(backend, value, "us", repetition=index, repetitions=3)
                        for index, value in enumerate(values))
        for phase, backends in (("first", ("vm", "luajit_interp", "luajit")),
                                ("compile", ("representative", "luajit"))):
            rows.extend(sample(backend, 3, "ms", phase, repetition=index, repetitions=3)
                        for backend in backends for index in range(3))
        aggregate = sample("cpp", 999999)
        aggregate.update(run_type="aggregate", aggregate_name="mean")
        names = list(dict.fromkeys(row["name"] for row in rows))
        result = comparison_report({"benchmarks": [*rows, aggregate],
                                    "context": {"luajit_revision": "pinned"}}, names, 3)
        self.assertIn("| sieve | 10000 | 2.000 | 10.000 | 5.000 | 2.000 | 5.00x | 2.00x | 5.00x |", result)
        self.assertIn("3000.000", result)
        self.assertIn("| sieve | 10000 | Vela | 100 | 150 | 100 | 200 | 80 |", result)
        self.assertIn("First invocation in a fresh state", result)
        self.assertIn("LuaJIT loads source into Lua bytecode", result)
        self.assertIn("luajit_revision: pinned", result)

    def test_converts_units_without_using_cpu_time(self) -> None:
        rows = [sample("cpp", 1000), sample("vm", 0.000010, "s"),
                sample("luajit_interp", 0.005, "ms"), sample("luajit", 2, "us")]
        result = comparison_report({"benchmarks": rows}, [case(backend) for backend in BACKENDS], 1)
        self.assertIn("| 1.000 | 10.000 | 5.000 | 2.000 | 10.00x | 2.00x | 5.00x |", result)

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

    def test_rejects_failed_nonfinite_or_false_jit_reports(self) -> None:
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
        rows[-1]["warm_traces"] = 0
        with self.assertRaisesRegex(ValueError, "no compiled traces"):
            comparison_report({"benchmarks": rows}, names, 1)
        rows = steady_samples()
        rows[-2]["traces"] = 1
        with self.assertRaisesRegex(ValueError, "interpreter reported"):
            comparison_report({"benchmarks": rows}, names, 1)
        with self.assertRaisesRegex(ValueError, "No supported"):
            comparison_report({"benchmarks": []}, [], 1)

    def test_explicit_backend_filter_reports_only_selected_backends(self) -> None:
        rows = steady_samples()[:2]
        result = comparison_report({"benchmarks": rows}, [row["name"] for row in rows], 1)
        self.assertIn("| 1.000 | 2.000 | - | - | 2.00x |", result)

    def test_documents_api_baseline_differences_only_for_selected_workloads(self) -> None:
        entries = [sample("cpp", 1000), sample("vm", 5000)]
        for entry in entries:
            entry["name"] = entry["name"].replace("sieve", "json_roundtrip")
        names = [entry["name"] for entry in entries]
        result = comparison_report({"benchmarks": entries}, names, 1)
        self.assertIn("pinned yyjson DOM arenas", result)
        self.assertIn("representation and contract differences", result)
        self.assertNotIn("ASCII character mapping", result)
        ordinary_entries = [sample("cpp", 1000), sample("vm", 5000)]
        ordinary = comparison_report({"benchmarks": ordinary_entries},
                                     [entry["name"] for entry in ordinary_entries], 1)
        self.assertNotIn("Native baseline contracts", ordinary)

    def test_comparison_options_preserve_google_benchmark_arguments(self) -> None:
        parsed = parse_arguments(["--compare", "--report", "out/test.json", "--",
                                  "--benchmark_repetitions=3"])
        self.assertTrue(parsed.compare)
        self.assertEqual(parsed.variant, "release")
        self.assertEqual(parsed.benchmark_arguments, ["--benchmark_repetitions=3"])

    def test_native_report_normalizes_batches_and_labels_subtraction_and_heap_limits(self) -> None:
        rows = native_samples(3)
        result = native_call_report({"benchmarks": rows}, sorted({row["name"] for row in rows}), 3)
        self.assertIn("| integer | 64 | 2.000 | 3.000 | 10.000 | 30.000 | 20.000 | 3/3/3/3 |", result)
        self.assertIn("not paired samples", result)
        self.assertIn("not cumulative allocation counts", result)
        self.assertIn("| integer | 64 | vm | 100 | 100 | 2 | 2 | 80 |", result)

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
        with self.assertRaisesRegex(ValueError, "invalid heap"):
            native_call_report({"benchmarks": rows}, names, 1)

    def test_native_runner_selects_native_registry_and_writes_report(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vela-native-bench-") as temporary:
            report = Path(temporary) / "native.json"
            parsed = parse_arguments(["--native-compare", "--report", str(report)])

            def execute(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess:
                self.assertIn("--benchmark_filter=^native/", command)
                rows = native_samples(7)
                if "--benchmark_list_tests=true" in command:
                    return subprocess.CompletedProcess(command, 0, stdout="\n".join(sorted({row["name"] for row in rows})))
                report.write_text(json.dumps({"benchmarks": rows}), encoding="utf-8")
                return subprocess.CompletedProcess(command, 0)

            with patch("tools.bench.build"), patch("tools.bench.subprocess.run", side_effect=execute):
                with redirect_stdout(io.StringIO()):
                    run(parsed)
            self.assertIn("VM native ns", report.with_suffix(".md").read_text(encoding="utf-8"))

    def test_runner_rejects_benchmark_error_even_when_process_succeeds(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vela-bench-") as temporary:
            report = Path(temporary) / "results.json"
            markdown = report.with_suffix(".md")
            report.write_text("stale JSON", encoding="utf-8")
            markdown.write_text("stale success", encoding="utf-8")
            parsed = parse_arguments(["--compare", "--report", str(report)])

            def execute(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess:
                self.assertFalse(report.exists())
                self.assertFalse(markdown.exists())
                if "--benchmark_list_tests=true" in command:
                    return subprocess.CompletedProcess(command, 0, stdout="\n".join(case(backend) for backend in BACKENDS))
                self.assertIn("--benchmark_enable_random_interleaving=true", command)
                rows = steady_samples(7)
                rows[-1].update(error_occurred=True, error_message="stack overflow")
                report.write_text(json.dumps({"benchmarks": rows}), encoding="utf-8")
                return subprocess.CompletedProcess(command, 0)

            with patch("tools.bench.build"), patch("tools.bench.subprocess.run", side_effect=execute):
                with self.assertRaisesRegex(ValueError, "stack overflow"):
                    run(parsed)
            self.assertFalse(markdown.exists())

    def test_runner_keeps_user_repetitions_and_uses_the_filtered_binary_registry(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vela-bench-") as temporary:
            report = Path(temporary) / "results.json"
            parsed = parse_arguments(["--compare", "--report", str(report), "--",
                                      "--benchmark_repetitions=3", "--benchmark_filter=^compare/"])

            def execute(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess:
                self.assertIn("--benchmark_filter=^compare/", command)
                if "--benchmark_list_tests=true" in command:
                    return subprocess.CompletedProcess(command, 0, stdout="\n".join(case(backend) for backend in BACKENDS))
                self.assertIn("--benchmark_repetitions=3", command)
                self.assertNotIn("--benchmark_repetitions=7", command)
                report.write_text(json.dumps({"benchmarks": steady_samples(3)}), encoding="utf-8")
                return subprocess.CompletedProcess(command, 0)

            with patch("tools.bench.build"), patch("tools.bench.subprocess.run", side_effect=execute):
                with redirect_stdout(io.StringIO()):
                    run(parsed)
            self.assertIn("Vela/LuaJIT", report.with_suffix(".md").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
