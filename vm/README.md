# Vela register VM bootstrap

The VM bootstrap uses one 32-bit instruction word with an 8-bit opcode and
three 8-bit operands. The current instruction set is intentionally small:
typed integer arithmetic, moves, null/boolean/integer constants, instance
allocation, field reads and writes, relative branches, calls, and return.

`Frame` owns no storage. It wraps a contiguous register span and registers the
span as a `runtime::RootRange`. The VM places up to 64 active frames in one
contiguous register backing allocation, so a register containing a heap object
is visible to the collector at every allocation safepoint in nested calls.

The interpreter validates a function before execution, then keeps the decoded
register pointer, program counter, code span, and isolate heap in local state.
The common dispatch path is a switch for portability; dispatch changes require
Release benchmarks before introducing a computed-goto variant.
