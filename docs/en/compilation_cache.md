# Native compilation cache

[**English**](compilation_cache.md) | [简体中文](../zh-CN/compilation_cache.md)

Vela caches native object code produced by the shared JIT backend. Source checking
still runs before using cached code. A cache entry contains neither a heap snapshot
nor an Isolate's globals, bindings or asynchronous work.

`vela run FILE` and `vela debug FILE` use `.cache/vela/native/run` beneath the
working directory. `vela test` uses `.cache/vela/native/test` beneath the project
configuration root. Development sessions accept `--code-cache=DIR`.

For embedding, set `jit::SourceOptions::compilation.code_cache` and call
`jit::CompileFile`, `CompileModules` or `CompileSource` with a `jit::Compiler`.
Leave the option unset for uncached compilation. `CompiledImage::cache` reports
`Disabled`, `Miss`, `Hit` or `Invalid`.

## Identity and checking

Each request loads its current sources, resolves modules, checks types and
diagnostic severities, specializes generics, prepares Typed IR and generates LLVM
IR. The key fingerprints that generated module together with the compiler build
identity, LLVM version, runtime ABI identity and native optimization setting.
The module includes its target, layouts, imports, function versions and source
metadata. Debugging modes and JIT tiers affect the generated module.

A hit skips LLVM optimization and object emission. It still links the object into
the requesting JIT and creates fresh runtime state when instantiated. It does not
skip source checking. Warnings remain visible on hits; changing a warning to an
error prevents execution even when a previous successful object exists.

File timestamps and lengths do not establish identity. Source edits, changed
dependencies, canonical module paths, generated SDK declarations and compiler
changes participate through the checked module or build identity. Interface
membership metadata uses canonical runtime-index order so hash iteration does
not create different keys for identical source.

## Object files and publication

Files are named `<key>.vlobj`. The `VELAJIT1` container stores its key and a
checksummed native object payload. The reader verifies the container and object
format before returning a hit. Missing or corrupt entries are recompiled from
the checked source. Read and write failures produce explicit diagnostics and
prevent that compilation request from succeeding.

Publication uses a temporary file in the destination directory and atomic
replacement. Competing writers publish whole entries. On Windows, replacement
uses `SetFileInformationByHandle(FileRenameInfoEx)` with replace/POSIX semantics.
Atomic visibility does not promise crash durability of directory metadata.

Cache objects are trusted local compiler artifacts. Checksums detect corruption;
they do not authenticate producers or make native code safe to load from an
untrusted party. The cache is specific to the compiler, ABI and target and is not
a distribution format.

## Ownership and distribution

A compiled JIT image retains its linked code and immutable metadata through a
shared owner. It can outlive the `jit::Compiler` and source AST. Each
`NativeInstance::Instantiate` binds the receiving Isolate's host functions,
validates their signatures, and creates independent globals and constant roots.
See [native embedding](native_embedding.md) for lifetime rules.

Use `vela build` for source-independent distribution: executables, native object
files or static libraries with their generated headers and manifests. The native
cache requires source files and does not replace those AOT artifacts. See
[AOT compilation](aot.md).

