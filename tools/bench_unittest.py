"""Check benchmark pairing, units, failures and command boundaries."""

from __future__ import annotations

import unittest
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import tempfile
from unittest.mock import patch

from tools.bench import parse_arguments, run
from tools.benchmark_report import comparison_report


def sample(backend: str, elapsed: float, unit: str = "ns") -> dict:
    return {
        "name": f"compare/{backend}/sieve/10000/real_time",
        "real_time": elapsed, "cpu_time": 999999999, "time_unit": unit,
        "baseline_bytes": 100, "retained_bytes": 100,
        "committed_bytes": 200, "register_bytes": 80,
    }


class BenchmarkReportTest(unittest.TestCase):
    def test_uses_wall_time_medians_converts_units_and_ignores_aggregates(self) -> None:
        aggregate = sample("cpp", 999999)
        aggregate.update(run_type="aggregate", aggregate_name="mean")
        compile_sample = sample("cpp", 3, "ms")
        compile_sample["name"] = "compile/representative/sieve/10000/real_time"
        result = comparison_report({"benchmarks": [
            sample("cpp", 1000), sample("cpp", 3, "us"), sample("cpp", 2000),
            sample("vm", 0.000008, "s"), sample("vm", 0.010, "ms"), sample("vm", 12, "us"),
            aggregate, compile_sample,
        ]})
        self.assertIn("| sieve | 10000 | 3/3 | 2.000 | 10.000 | 5.00x |", result)
        self.assertIn("3000.000 |", result)
        self.assertIn("| sieve | 10000 | 100 | 100 | 200 | 80 |", result)

    def test_rejects_failed_or_unpaired_reports(self) -> None:
        failed = sample("vm", 1000)
        failed.update(error_occurred=True, error_message="checksum mismatch")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            comparison_report({"benchmarks": [sample("cpp", 1000), failed]})
        with self.assertRaisesRegex(ValueError, "missing C\\+\\+ or VM"):
            comparison_report({"benchmarks": [sample("vm", 1000)]})
        with self.assertRaisesRegex(ValueError, "No paired"):
            comparison_report({"benchmarks": []})
        with self.assertRaisesRegex(ValueError, "invalid wall time"):
            comparison_report({"benchmarks": [sample("vm", float("nan"))]})

    def test_comparison_options_preserve_google_benchmark_arguments(self) -> None:
        parsed = parse_arguments(["--compare", "--report", "out/test.json", "--",
                                  "--benchmark_repetitions=3"])
        self.assertTrue(parsed.compare)
        self.assertEqual(parsed.variant, "release")
        self.assertEqual(parsed.benchmark_arguments, ["--benchmark_repetitions=3"])

    def test_runner_rejects_benchmark_error_even_when_process_succeeds(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vela-bench-") as temporary:
            report = Path(temporary) / "results.json"
            markdown = report.with_suffix(".md")
            report.write_text("stale JSON", encoding="utf-8")
            markdown.write_text("stale success", encoding="utf-8")
            parsed = parse_arguments(["--compare", "--report", str(report)])

            def execute(command: list[str], **_kwargs: object) -> None:
                self.assertFalse(report.exists())
                self.assertFalse(markdown.exists())
                self.assertIn("--benchmark_enable_random_interleaving=true", command)
                failed = sample("vm", 1)
                failed.update(error_occurred=True, error_message="stack overflow")
                report.write_text(json.dumps({"benchmarks": [failed]}), encoding="utf-8")

            with patch("tools.bench.build"), patch("tools.bench.subprocess.run", side_effect=execute):
                with self.assertRaisesRegex(ValueError, "stack overflow"):
                    run(parsed)
            self.assertFalse(markdown.exists())

    def test_runner_keeps_user_repetitions_and_writes_paired_summary(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vela-bench-") as temporary:
            report = Path(temporary) / "results.json"
            parsed = parse_arguments(["--compare", "--report", str(report), "--",
                                      "--benchmark_repetitions=3"])

            def execute(command: list[str], **_kwargs: object) -> None:
                self.assertIn("--benchmark_repetitions=3", command)
                self.assertNotIn("--benchmark_repetitions=7", command)
                report.write_text(json.dumps({"benchmarks": [sample("cpp", 1000),
                                                            sample("vm", 5000)]}), encoding="utf-8")

            with patch("tools.bench.build"), patch("tools.bench.subprocess.run", side_effect=execute):
                with redirect_stdout(io.StringIO()):
                    run(parsed)
            self.assertIn("5.00x", report.with_suffix(".md").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
