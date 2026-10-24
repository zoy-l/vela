# Luau reference notes

Vela uses the local Luau checkout as an implementation reference for runtime
engineering. It does not copy Luau's Lua compatibility, dynamic table
semantics, or public API.

The relevant ideas are:

- the compiler produces bytecode separately from the VM, allowing the runtime
  deployment to omit the compiler;
- the interpreter uses compact fixed-width instructions, register-like stack
  slots, local hot state, and direct dispatch on Clang/GCC;
- compiler work includes constant folding, value tracking, jump cleanup,
  call-target feedback, and bytecode validation;
- the VM has explicit slow paths for calls that can reallocate stacks or invoke
  the collector, while the common path keeps pointers and frame state local;
- performance changes are checked with Release benchmark runs rather than
  inferred from source-level structure.

Vela differs at the semantic boundary: types are required, class fields and
host calls are statically resolved, and dynamic values are explicit escape
paths rather than the representation of every primitive value.
