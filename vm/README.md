# Vela register VM

The VM uses 32-bit instructions with an 8-bit opcode and three 8-bit operands.
Some instructions combine two operands into a signed 16-bit immediate or jump
offset. IR and bytecode share the opcode definitions in `base/opcode.h`.
Pool and field indexes use a preceding index-extension word when they exceed
their compact operand. Extended indexes span the full uint32 range; register
operands remain eight bits. Jumps, handlers, and source mappings refer to word
offsets and cannot enter the middle of an extended instruction.

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
256 slots per frame and 1024 active frames. Frame metadata and register files
have stable addresses and are allocated as deeper calls are reached; a single
frame is stored inline. Native backends also check the current thread's physical
stack boundary and return `StackOverflow` before exhausting its reserved space.

The switch interpreter trusts validated code structure while retaining dynamic
type, bounds, arithmetic, allocation, and stack-depth error handling. Entry
validation rejects invalid handles, indexes, and mismatched Isolates. Returned
heap values are borrowed; hosts must root them before further allocation or GC.

See [execution contracts and measurements](../docs/en/vm_execution.md) for
allocation, optimization, encoding limits, and Release results. Dispatch or
value-representation changes require representative measurements.
