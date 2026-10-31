# Debugger

[**English**](debugger.md) | [简体中文](../zh-CN/debugger.md)


`vela debug FILE` JIT-compiles a native snapshot with instruction inspection,
then pauses before the first instruction of zero-argument `main`. Script output
uses stdout; debugger prompts and inspection use stderr. A completed program
uses the ordinary script exit code. `quit` or EOF stops execution and exits `0`.

Commands:

| Command | Behavior |
| --- | --- |
| `continue`, `c` | Run to a breakpoint or completion |
| `step`, `s` | Stop at the next source line, entering Vela calls |
| `next`, `n` | Step over calls in the current frame |
| `finish`, `f` | Run until the current frame returns |
| `stepi`, `si` | Advance one emitted IR instruction |
| `break [FILE:]LINE`, `b` | Set a source breakpoint; omitted FILE uses the selected frame |
| `break FUNCTION` | Break on entry to a named function or method |
| `breakpoints` | List breakpoint IDs and resolved locations |
| `delete ID`, `delete all` | Remove breakpoints |
| `stack`, `bt` | Show frames, PCs and call-site source positions |
| `frame DEPTH` | Select a frame for inspection; zero is the current frame |
| `registers`, `regs` | Show available IR values in the selected frame |
| `print rN` | Inspect one register without executing code |
| `field rN INDEX` | Read an object's field by its layout index |
| `element rN INDEX` | Read an array element |
| `help`, `h`; `quit`, `q` | Show commands; stop the program |

Full source paths and unique basenames are supported; ambiguous basenames require
a full path. Quote paths containing spaces. A requested line without an emitted
instruction moves to the next executable line in that file; the resolved location
is printed. Loop back edges can hit the same line again. Function breakpoints use
the names displayed in the stack. Stepping always controls the currently executing
frame, regardless of which caller is selected for inspection.

The debugger uses Typed IR after shared optimization. Eliminated statements have no
stopping location. Register inspection shows available IR values and temporaries,
not machine registers or source-local variable names. Instruction inspection disables
object allocation elimination so inspectable objects retain real fields. It does not evaluate arbitrary expressions,
modify variables, pause inside native C++ functions or interrupt a running continue
command asynchronously. Native calls are atomic from the instruction observer's
perspective.

## Native interface and lifetime

Embedding hosts implement `runtime::NativeDebugger::AtSafepoint` and install the
observer with `isolate.native_control().SetDebugger(&debugger)`.
Compile with `NativeDebugging::Instructions` when per-instruction values are needed.
Normal development safepoints and source-line debugging provide their own stopping
granularity; installing an observer does not regenerate already compiled code.

`NativeDebugContext` and `NativeDebugFrame` are borrowed views valid during the
callback. Frames expose the source text, location, immutable function identity
and tier of the executing version. `ReadRegister` returns an empty optional for
unavailable values or invalid indices. The frame at depth zero is current;
caller positions identify their calls, including lazy global initialization.

Paused native frames retain precise GC roots. The observer may allocate or collect
on the same Isolate; copied references that outlive the callback need
`RootedValue`. The Isolate, execution and debugger must remain alive, and the host
must detach the observer before destroying it. Keep it installed while pumping
asynchronous main and background work so resumed functions remain debuggable.

`NativeDebugContext::RequestStop()` requests `ExecutionStopped`. Stopping is not
a script exception and does not run pending `finally` blocks or roll back prior
side effects. Frame roots detach during unwinding. A stopped run is replaced
through a fresh instance/Isolate or the development session's Reload lifecycle;
the debugger does not resume a discarded native stack.

Ordinary execution uses its configured safepoints without allocating an
instruction-inspection value array. Native C++ calls cannot be single-stepped
inside the host body. A host can post stop requests through the thread-safe native
safepoint endpoint; the interactive console itself reads commands only while paused.

## Verification

Language and command tests cover function and loop breakpoints, step/next/finish,
caller locations, value/field/array inspection, GC during observation, exception
unwinding and stopping. Async command tests exercise a breakpoint after suspension
and verify that quitting does not become a scheduler deadlock. Reloadable JIT tests
cover source versions and deoptimization in both tiers.

