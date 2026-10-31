# Byte storage and typed views

[**English**](byte_data.md) | [简体中文](../zh-CN/byte_data.md)

`ByteArray` owns fixed-length, contiguous byte storage. `ByteData` is a typed
read/write view over that storage and keeps its owner alive. The API uses
Dart-style multi-byte accessors and explicit endianness, retaining the Vela name
ByteArray. It does not introduce ByteBuffer or depend on the
[JSON API](json.md).

```vela
func main(): void {
  const bytes = ByteArray(length: 8);
  const data = ByteData.view(bytes);

  data.setInt32(0, 42, endian: Endian.little);
  const result: int = data.getInt32(0, endian: Endian.little);
  print(result);

  const copy = bytes.slice();
  copy[0] = 0;
  print(bytes[0]); // 42: slice has independent storage.
}
```

The first version covers memory only. It adds no resizable byte array, detached
buffer, native-pointer wrapping, shared-memory concurrency, encoding codec, file,
stream, or network API. ByteData has no array subscript syntax.

## ByteArray API

| API | Contract |
| --- | --- |
| `ByteArray(length: int)` | Allocate exactly length zero-initialized bytes |
| `ByteArray.from(values: int[]): ByteArray` | Validate and copy integer elements into a new byte array |
| `bytes.length: int` | Read-only byte count; O(1) |
| `bytes[index]: int` | Read one unsigned byte as an int; O(1) |
| `bytes[index] = value` | Check bounds and `0 <= value <= 255`, then store one byte |
| `bytes.slice(start?: int, end?: int): ByteArray` | Copy the selected byte range into independent storage |

ByteArray is a dedicated reference type, not an alias or subtype of `int[]`.
Its storage uses one byte per element, not an integer or Value slot. Length is
fixed for the owner's entire lifetime, including when no view exists. There is
no writable length, capacity, push, pop, reserve, or other resizing operation.
Length zero is valid. Negative lengths fail; positive sizes are checked against
host and heap allocation limits before allocation, without truncation or wrap.

Element access accepts int indices and values. There is no clamping, modulo
reduction, numeric coercion, or negative indexing. `from` rejects any out-of-range
element; it does not truncate integer values as some typed-array APIs do. The
source array remains unchanged, and no partially constructed result is returned.

`slice` shares endpoint rules with [ordinary arrays](arrays.md): omitted start is
zero, omitted end is length, negative endpoints count from the end, and endpoints
clamp to `[0, length]`. An end before start yields an empty array. The normalized
half-open range is copied; even a whole-array slice does not share storage.
Endpoint normalization must avoid overflow for extreme int values.

`from` explicitly copies from int[]. Copying back requires explicit indexed
reads into an int[]; no implicit conversion, representation cast, or shared
backing storage is provided. `from` requires a statically checked int[];
nullable integer arrays and arrays of other element types are rejected.
This feature does not add dynamic values or mixed containers.

## Endian and ByteData API

```vela
enum Endian {
  big,
  little,
  host,
}
```

| API | Contract |
| --- | --- |
| `ByteData.view(bytes: ByteArray, offset?: int, length?: int): ByteData` | Create a shared view; default offset is zero and default length is the remaining bytes |
| `data.byteLength: int` | Read-only length of the view in bytes; O(1) |
| `data.getUint8(offset: int): int` | Read one unsigned byte |
| `data.setUint8(offset: int, value: int): void` | Write one checked unsigned byte |
| `data.getInt8(offset: int): int` | Read one signed byte |
| `data.setInt8(offset: int, value: int): void` | Write one checked signed byte |

Multi-byte methods use these signature patterns, with the suffixes listed below:

```text
getXxx(offset: int, endian?: Endian): int
setXxx(offset: int, value: int, endian?: Endian): void

getFloat32(offset: int, endian?: Endian): double
setFloat32(offset: int, value: double, endian?: Endian): void
getFloat64(offset: int, endian?: Endian): double
setFloat64(offset: int, value: double, endian?: Endian): void
```

| Suffix | Width in bytes | Integer write range / floating representation |
| --- | --- | --- |
| `Int8` | 1 | `[-128, 127]`; no endian parameter |
| `Uint8` | 1 | `[0, 255]`; no endian parameter |
| `Int16` | 2 | `[-32768, 32767]` |
| `Uint16` | 2 | `[0, 65535]` |
| `Int32` | 4 | `[-2147483648, 2147483647]` |
| `Uint32` | 4 | `[0, 4294967295]` |
| `Int64` | 8 | Entire signed 64-bit int range |
| `Float32` | 4 | IEEE 754 binary32; Vela argument/result is double |
| `Float64` | 8 | IEEE 754 binary64 |

All multi-byte methods default to `Endian.big`, matching Dart ByteData. `host`
resolves the execution platform's byte order; it does not promise a portable
external representation. Eight-bit methods have no endian argument. Uint64 is
excluded because Vela int cannot represent its complete range.

ByteData cannot be constructed independently of a ByteArray. There are no
public setters for its owner, base offset, or length. Optional parameters use
ordinary omitted-argument rules; omission does not accept an explicit null.
Calls support Vela's normal positional and named arguments.

## View bounds and numeric behavior

For an owner of length `N`, view creation requires `0 <= offset <= N` and
`0 <= length <= N - offset`. These are strict bounds: view offsets do not use
slice normalization or clamping. An empty view at offset N is valid. All offsets
are measured in bytes, and each accessor's offset is relative to its view.

For an access of width W, first check a nonnegative offset, `W <= byteLength`,
and `offset <= byteLength - W`. This establishes the intended
`offset + W <= byteLength` without overflowing addition. Only then calculate the
owner address. Access to an empty view fails for every supported width.

Reads and writes support unaligned offsets. Integer reads use unsigned or
two's-complement signed interpretation according to the method. Setters reject
out-of-range integers instead of truncating or wrapping. Bounds and value
checks complete before the first byte is changed, so a failed setter leaves
all shared storage unchanged.

Float32 stores use IEEE 754 round-to-nearest, ties-to-even conversion from
binary64 to binary32; reads widen binary32 to double. Finite overflow can produce
infinity, and subnormal values and underflow follow IEEE 754. Float64 stores
and reads use binary64. Both widths accept NaN, infinity, and signed zero;
NaN payload identity is not a language guarantee. These binary operations do
not inherit JSON's restriction to finite numbers. Int/double argument conversion
remains explicit.

Use byte copies, unsigned assembly, and bit-preserving conversions for the C++
implementation. Do not form unaligned typed pointers, violate aliasing rules,
overflow signed arithmetic, or rely on out-of-range signed casts.

## Ownership, mutation, and GC

Creating a ByteData view is O(1) and copies no bytes. The view contains a strong
reference to its ByteArray owner plus an immutable byte offset and length. It
must not retain only a raw address. A live view, including an empty view, keeps
the whole owner allocation alive after the original variable leaves scope.

Overlapping views observe each other's writes immediately on the owning Isolate
thread. Writes through the ByteArray and through its views address the same
storage. `slice` always creates a separate owner. No atomic access or concurrent
host-thread mutation contract is introduced.

ByteArray owns and releases its backing allocation exactly once. Its size and
any external backing allocation participate in heap accounting and collection
pressure. GC treats raw bytes as non-reference data and never scans their
contents for pointers. ByteData tracing visits its owner and uses the existing
reference publication barrier. Construction and copying root all owners and
new objects before allocation safepoints. Collecting one view does not release
storage still owned by a live ByteArray or another view.

The current non-moving collector and fixed owner length allow stable storage,
but stability is not a substitute for rooting. No public native borrowing or
pointer API is added in this version.

## Errors and type rules

| Failure | Runtime status / behavior |
| --- | --- |
| Invalid ByteArray index, view range, or accessor range | Existing `IndexOutOfBounds` |
| Negative allocation length or out-of-range integer value | Existing `InvalidNumericConversion` |
| Allocation or representable-storage exhaustion | Existing recoverable `OutOfMemory` |
| Wrong static argument, index, value, or endian type | Compile-time diagnostic |
| Wrong runtime operand in a checked builtin | Existing TypeError path |

The status names above identify existing execution paths; Error.code continues
to use the established status-to-string mapping. Errors remain catchable through
the ordinary native exception mechanism, subject to existing error-allocation
failure behavior. No invalid operation returns zero or silently ignores a write.

ByteArray and ByteData support ordinary references, nullable annotations,
parameters, returns, fields, captures, and typed containers. Their runtime
identities stay distinct from int[], ordinary instances, and each other.
Only ByteArray provides indexed access. `length` and `byteLength` are read-only
built-in properties; member access cannot fabricate fields or bypass checking.
Endian remains an enum, not an interchangeable integer selector.

## Integration and validation

ByteArray and ByteData have dedicated frontend type kinds and runtime object
kinds, using the existing object-valued Value representation. The protected
standard environment supplies factory namespaces and the Endian enum. Constructor,
property and method signatures share builtin metadata with completion, hover,
named arguments and source tools. Aliases, nullable types, fields, captures,
arrays, collections and Future payloads retain their normal type rules.

Instance method values bind the receiver and retain it through GC. With no
function-type context they use the full positional signature, including Endian;
a context such as `(int) => int` can select getInt32 with its default big order.
Static factory values have fixed full positional signatures. Explicit null does
not stand for omitted int or Endian arguments.

Each owner is one GC allocation containing its object header and exactly length
raw bytes; it uses neither int[] layouts nor growable-array capacity machinery.
The current heap allocation-size representation bounds a single allocation to
uint32 bytes including its header; larger requests follow OutOfMemory. A view
stores a strong owner pointer, base offset and fixed length. Its owner reference
is initialized before the existing GC publication barrier. ByteArray is a GC
leaf; ByteData traces its owner. Storage contributes to ordinary heap accounting
and is reclaimed once the owner becomes unreachable.

The compiler lowers byte subscripts and multi-byte operations to dedicated
checked builtins. Ordinary array indexing retains its own native lowering path. Reads, writes, failures and allocations remain observable through the
existing IR effect and exception paths. Default arguments are filled before
source-order evaluation. Null-safe access skips indices and arguments when the
receiver is null. Builtin results are published only after success, including
when the result replaces an aliased input.

Native layout metadata preserves byte object-kind constraints in typed arrays,
collections and runtime type tests. Owners, views and backing bytes are created
per instance during execution. JIT object-cache identity includes generated
metadata and the runtime ABI; AOT distributes native artifacts with manifests.
REPL submissions retain checked types and layouts through the development
session. Debugger output identifies ByteArray length and ByteData byteLength.
The runtime has no dependency on compiler or tooling.

Regression tests cover zeroing, empty owners and views, copying and slice
normalization, readonly properties, rejected resizing, every integer width and
endpoint, big/little/host order, unaligned access, view-relative and extreme
bounds, unchanged storage after failure, IEEE reference vectors, signed zero,
NaN/infinities, and overlapping views. Float32 rounding is checked under changed
host rounding modes without modifying that environment. GC tests retain only a
view, including an empty view, and verify final storage release. Compiler,
source-free fresh-Isolate images, async suspension, typed containers,
REPL, LSP, formatting and generated documentation use the production paths.

Run [`examples/byte_data.vela`](../../examples/byte_data.vela) for the public API.
Release benchmark workloads measure zeroing/allocation, indexed access,
copying, aligned/unaligned typed access, byte orders, view creation and GC
pressure. See [measurement boundaries](../../benchmarks/README.md) and
[retained aggregate evidence](../../benchmarks/results/rolling_summary.json).

Run the native byte benchmarks for allocation, copies, typed access and views.
Check results and post-GC retention separately from timing. Historical interpreter
measurements do not establish JIT latency; current aggregate evidence and exact
boundaries belong in the [rolling summary](../../benchmarks/results/rolling_summary.json).

## References

- [Dart Uint8List](https://api.dart.dev/dart-typed_data/Uint8List-class.html), [ByteData](https://api.dart.dev/dart-typed_data/ByteData-class.html), and [Endian](https://api.dart.dev/dart-typed_data/Endian-class.html).
- [JavaScript Uint8Array](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Uint8Array) and [DataView](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/DataView).
- Vela [arrays](arrays.md), [garbage collection](garbage_collection.md), and [compilation cache](compilation_cache.md).
