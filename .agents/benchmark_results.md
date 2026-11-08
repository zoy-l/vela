# Benchmark Result Retention

These rules govern benchmark results retained as durable project evidence.

- Keep one tracked summary at `benchmarks/results/rolling_summary.json` with fixed `previous` and
  `latest` entries. Do not append a history of runs or create date-named result files.
- Store aggregate results only: per-workload median and variation, plus comparison ratios and
  relevant heap or register counters when the suite reports them. Do not commit per-iteration raw
  samples. Exploratory raw output under ignored `out/` or `.cache/` remains disposable.
- Each entry records the source revision and working-tree state, run date, OS and architecture, CPU,
  compiler/toolchain, Release configuration, benchmark suite identity, exact command and options,
  repetition count, and whether all expected cases and correctness checks passed.
- Entries use `schema_version: 1`. `workload_pairs`, `native_call_pairs`, and
  `native_class_cpp_pairs` identify backend cases in `cases`, retain backend medians and `wall_cv`,
  and store comparisons in `ratios_to_cpp`. Workload medians are nanoseconds per complete iteration;
  native-call medians are nanoseconds per call. `ratio_summary` groups the same backend statistics
  under `complete_workloads`, `native_free_calls`, and `native_class_calls` in both entries.
- When `latest.comparison.previous_summary_sha256` is present, it hashes the retained `previous`
  entry as UTF-8 JSON with `ensure_ascii=False`, sorted keys, separators `(',', ':')`, and no trailing
  newline. Record that encoding in `previous_summary_encoding`. Remove unverified predecessor
  hashes and mark their comparisons as not comparable; do not reconstruct missing measurements.
- Promote a run only after the complete intended suite succeeds. On promotion, move `latest` to
  `previous` and write the new summary to `latest`; failed or incomplete runs leave both entries
  unchanged. Publish the update atomically.
- Compare the two entries directly only when their machine, toolchain, build, suite, and measurement
  settings match. Otherwise retain the metadata but mark the comparison as not comparable.
- Keep only these two entries in the repository. Additional history or raw samples require an
  explicit user request and a separately defined retention target.
