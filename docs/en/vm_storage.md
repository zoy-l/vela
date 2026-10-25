# VM storage, constants and dispatch

[**English**](vm_storage.md) | [简体中文](../zh-CN/vm_storage.md)


Vela uses split storage for VM registers: a contiguous 8-byte payload array
and one `ValueKind` byte per slot. Primitive instructions read/write native
int64/double/bool payloads directly, and call argument windows copy payloads and
tags together. Each retained register therefore needs 9 bytes, compared with the
16-byte public `runtime::Value`. The public host value and ordinary class fields
retain their existing representation. This is not NaN boxing: every int64 value
and every double bit pattern, including NaN payloads and negative zero, remains
representable.

Payloads use aligned byte storage and fixed-size `memcpy` at the typed boundary,
preserving object representations without inactive-union-member access. Clang
optimizes these copies to ordinary native loads and stores on the hot path.

Slots may be reused for different static types across lifetimes. Tags describe
their current representation; the precise collector scans object/string tags
only. Reused frames clear tags before registration, preventing references from
earlier invocations from staying alive. Primitive payload bytes need not be
cleared on reuse. Register views borrow payload/tag arrays and cannot outlive
the active frame; slot-reference assignment writes a value rather than rebinding
the reference. Moving a slot reference preserves that contract.

Builtins materialize at most three public values at their input boundary. Native
callbacks materialize only their actual argument count in a reusable VM-owned
buffer, which grows only when a larger call window is first encountered.
Explicit numeric conversions use the shared single-value conversion boundary,
avoiding the generic three-value argument array and generic builtin dispatch.
The host builtin path uses the same range/type checks; no conversion rule changes.
There is no repeated heap allocation or full 256-slot initialization. The actual
register window remains rooted throughout allocation and callback execution.

## Constants

Small integer literals use the signed int16 immediate fast path. Other int64
constants use a deduplicated, immutable function pool; integer folding can now
materialize the complete checked int64 range. Double pools deduplicate bit
patterns and preserve negative zero. Strings share an immutable, precisely rooted
program pool; compiler lookup uses hashing rather than repeated linear scans.

Integer, double and string constant loads use an unsigned 16-bit pool index,
supporting indexes 0..65535. Loading snapshots numeric pools and string roots,
and validates indexes once. Compiler encoding diagnoses larger indexes before
narrowing. Class/layout/field/call operands retain their independently checked
byte limits. No out-of-range constant is truncated or interpreted as an immediate.

## Copies, jumps and hot execution

Copy propagation uses block-local value identities and current representatives.
Overwriting a representative invalidates it without changing copies' value
identities. Explicit call windows retain their contiguous layout. Dead moves
then disappear through ordinary liveness elimination; physical self-move
elimination remains necessary after allocation. Jump threading resolves chains
in linear time, preserves infinite jump cycles, removes jumps to the following
instruction, and remaps source/exception metadata during compaction.

Direct-call entry and return, typed arithmetic, and dispatch are kept in one
compiler optimization boundary with Clang's always_inline attribute. Slow
allocation, native calls, and exception materialization remain separate. Field
access resolves the fixed offset directly with the existing layout bound check;
primitive stores avoid calling the GC write barrier. Reference stores keep their
barrier. There is no field-name hash lookup or runtime overload search.

