# Vela benchmarks

These workloads exercise the current register VM through the same compiler and
runtime path used by `vela run`. They intentionally stay within the bootstrap
language boundary: integer control flow, direct function calls, fixed-layout
fields, and allocation.

Run the complete set in a project-managed Release build:

```powershell
python -m tools.vl bench
```

Run one workload with a specific number of executions:

```powershell
python -m tools.vl bench benchmarks/function_calls.vela --iterations 2000
```

The runner reports compile time separately from the first execution and the
average of the remaining executions. It also prints the final result and the
number of live heap objects in the isolate.
