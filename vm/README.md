# Vela register VM

The VM uses 32-bit instructions with an 8-bit opcode and three 8-bit operands.
Some instructions combine two operands into a signed 16-bit immediate or jump
offset. IR and bytecode share the opcode definitions in `base/opcode.h`.

`Program::Create` takes ownership of function storage and validates bytecode.
`Program::Load` copies borrowed `Function` views before validation. Successful
programs expose immutable storage; execution does not revalidate instructions.
`FunctionHandle` retains its program, code, metadata, and rooted string pool.
The owning Isolate must outlive all programs and handles, on its own thread.

Each active `Frame` borrows a contiguous register span and registers that span
as a `runtime::RootRange`. The VM caches register storage separately for each
call depth, allocating only the function's required slots and reusing that
capacity on later calls. Slots are cleared before registering a new frame;
normal returns and error exits release all frame roots. The current limits are
256 slots per frame and 64 active frames.

The switch interpreter trusts validated code structure while retaining dynamic
type, bounds, arithmetic, allocation, and stack-depth error handling. Entry
validation rejects invalid handles, indexes, and mismatched Isolates. Returned
heap values are borrowed; hosts must root them before further allocation or GC.

See [execution contracts and measurements](../docs/en/vm_execution.md) for
allocation, optimization, encoding limits, and Release results. Dispatch or
value-representation changes require representative measurements.
