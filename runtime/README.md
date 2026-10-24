# Vela runtime foundation

The runtime currently provides the object and memory contract that the VM will
use. Each `Isolate` owns one `Heap`; there is no process-global heap or locking
on the allocation path.

Instances have a fixed `ClassLayout`. The layout records the fields that hold
references, so marking visits reference fields directly instead of inspecting
every field with a dynamic type lookup. The object payload is contiguous after
the header.

The collector is a precise, non-moving, incremental mark-and-sweep collector.
Roots are registered as `RootRange` objects around VM frame slots, globals, or
host handles. Field writes go through `Heap::SetField`, which applies the write
barrier while marking. The gray work list is intrusive and does not allocate
while a collection is running.

Any object that must survive an allocation safepoint must already be reachable
through a registered root range or another marked object. Raw `HeapObject*`
values are deliberately not implicit roots; VM lowering will keep temporary
values in registered frame slots before it calls the allocator.

Small objects use size-class free lists. `Value` is currently a general 16-byte
tagged value for the bootstrap runtime; typed VM registers and primitive arrays
will use narrower representations when the bytecode layer is added.

The heap is isolate-local and single-threaded. A future multi-threaded host API
must register roots through an isolate boundary instead of accessing the heap
directly.
