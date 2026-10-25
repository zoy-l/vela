# Exceptions

`throw` accepts a non-null class or interface value. Catch clauses test concrete
class identity or explicit interface implementation in source order. Values and
numbers are never converted to error objects implicitly.

```typescript
import "std:error";

class ValidationError {
  public param message: string;
}

func validate(value: int): int {
  if (value < 0) { throw ValidationError(message: "negative value"); }
  return value;
}

func main(): int {
  try {
    return validate(-1);
  } catch (error: ValidationError) {
    print(error.message);
    return 42;
  } catch (error: Error) {
    print(error.message);
    return 1;
  } finally {
    print("finished");
  }
}
```

A final `catch { ... }` handles any exception without exposing an untyped value.
Typed bindings require `catch (name: Type)`. Duplicate types and catch clauses
after a catch-all are rejected. A try must have catch, finally, or both. Catch
bindings follow ordinary lexical scope and mutable closure-capture rules.

The built-in `std:error` module exports `Error(message: string, code: string)`;
`code` defaults to `"Error"`. This is an ordinary fixed-layout class. VM failures
are materialized as this type: `message` contains the diagnostic, while `code`
contains `ExecutionStatusName`, for example `"division by zero"` or
`"native function failed"`. Custom error types require no inheritance or common
base class. Error is available through an explicit import, including prefixed
imports. Native failures and runtime checked operations use the same catch path.

Finally executes on normal completion, return, break, continue, and exception
propagation. Return expressions are evaluated once before finally; changing a
local in finally cannot replace the previously computed return value. A return,
break, continue, or throw inside finally replaces the pending completion. Errors
inside a finally block are handled by outer regions, never by that same try's
catch clauses. Only finalizers exited by a break/continue run; leaving a loop
inside a try does not prematurely exit the try. Finally resolves names in its
own lexical scope, regardless of shadowing inside the protected body.

Compiler checks conservatively discard non-null refinements on catch entry,
because any preceding operation may have modified captured or local state.
Constructor analysis runs finalizers before checking return initialization and
before propagating break/continue initialization states. A catch/finally
exceptional entry cannot assume fields written only inside the try body.

Unhandled results retain the original runtime status, or `Thrown` for explicit
script throws, plus owned source-aware traces. Automatic propagation through
finally preserves the original throwing frames, even when finally calls other
functions or handles another error. An explicit `throw value` starts a new throw
site. A failed lazy initializer resets only the frames actually unwound, so a
caller that catches the error can retry initialization.

## Representation and performance

The implementation follows the JVM's
[exception tables and explicit frame unwinding](https://docs.oracle.com/javase/specs/jvms/se25/html/jvms-2.html#jvms-2.10).
IR instructions record their innermost handler. CFG construction includes
exceptional successors before liveness, dead-code elimination, and register
allocation. Compaction remaps handler targets along with jumps. Encoding creates
sorted, disjoint protected ranges; loading validates every range and Catch entry.
The VM consults these tables by binary search only on an error. Ordinary dispatch
does not push/pop handler records and registers no pending-exception root.

A hidden GC allocation owns the thrown instance, compact function/PC positions,
status, and diagnostic text. Metadata and text share one allocation and count
toward heap pressure. Catch exposes the typed payload. Pending wrappers remain
ordinary precise roots while finally executes; nested exceptions need no global
exception stack, leaked host records, or longjmp. Replacing a pending completion
simply makes the old wrapper collectible. True allocation exhaustion can prevent
creating an error envelope; it is returned directly to the host as OutOfMemory.

`exception_no_throw.vela` compares protected integer computation with the existing
integer loop; `exception_caught.vela` measures 1,000 throws and typed catches.
Final Release measurements belong to the P2 performance report, not estimates.
