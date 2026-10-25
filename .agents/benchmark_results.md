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
- Promote a run only after the complete intended suite succeeds. On promotion, move `latest` to
  `previous` and write the new summary to `latest`; failed or incomplete runs leave both entries
  unchanged. Publish the update atomically.
- Compare the two entries directly only when their machine, toolchain, build, suite, and measurement
  settings match. Otherwise retain the metadata but mark the comparison as not comparable.
- Keep only these two entries in the repository. Additional history or raw samples require an
  explicit user request and a separately defined retention target.
