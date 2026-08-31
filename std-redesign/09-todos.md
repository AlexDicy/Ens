# Open questions and deferred items

## Blocking decisions

All five were decided on 2026-08-05 and folded into the other documents; nothing blocks implementation.

## Still to write

The prelude mechanism for `print`/`eprint`.
The migration order is written: see 10-migration-plan.md.

## Implementation work items surfaced by the design

Shortest round-trip float formatting, for `StringBuilder.append(double)` and interpolation; exists nowhere today.
Number formatting beyond decimal (radix, width, padding) and parsing/formatting for `decimal`.
A modification counter in `Map`, `Set`, `SortedMap`, and their views, aborting on mutation during a walk.
The compiler's rejected-key-type list for map and set keys: arrays, external handles, mutable collections, and function types.
Function types are the verified case (2026-08-30): `Set<(int) -> char>` compiles, because the `Hashable` bound is treated as universally satisfied, and the elements then hash by object identity, so two identical lambdas written at two sites are two keys while a capturing lambda evaluated twice is also two keys.
Writing `hash()` on a function value directly is already refused, so the generic path is the inconsistency, and the real work is making `Hashable` a bound with an exclusion list rather than special-casing one type.
External handles are refused by code generation as of 2026-08-30, in both the `Handle` and the `Handle?` spelling, with a message naming the type; sema still accepts the call, so making the bound honest is what moves the refusal to where it belongs.
Ratified 2026-08-30: keep the direct rejection and make the bound honest, so the generic path refuses too, rather than granting every value an identity.
A function value's identity is not stable under the compiler's freedom to share one closure object per capture-free lambda site or to allocate a fresh one per evaluation, so equality on it would answer questions about code generation rather than about the function; a class holding the function is how a program that needs identity gets it.
The work belongs to C4, which designs the containers and their key rules.
A test that ARC releases live locals on the throw path.
A pinning fixture for file-next-to-folder module resolution, plus a spec line.
A fixture for a cross-package subclass calling a protected constructor.
The 23 `new Error(...)` sites that break when `Error` goes abstract, most of which should throw `TestFailure`.
OS-level redirection for `run(captureOutput: true)`, wait-with-timeout, and kill in the native bridges.
The toString marker flip: after the Phase B seed and the C6 text rewrite give `StringBuilder` its `export override toString()`, Phase D makes an unmarked class method named `toString` an error, closing the A4 transition rule (ratified 2026-08-28).
Ratified 2026-08-30 and built: `new T[n]` in a generic body is judged once per instantiation through the obligations pass, and the containers hold their slots in `@std.collections.rawarray.RawArray<T>`, a package-wide struct over one `T[]` whose methods the compiler lowers to the array operations they stand for.
Reading a slot nothing has written is the one unchecked operation in the language and is reachable only from `@std`, by the visibility ladder rather than by a compiler rule; a container elsewhere holds `T?[]` and pays a tag on each slot.
The exemption is the `std.collections.rawarray` module rather than any one declaration in it, which is what let the store move to `RawArray<T>.allocate(n)` once the v0.2.1-beta seed carried that exemption.
`Map.keys` and `Map.values` still copy each element twice, once into a capacity-reserved list and once into the array it hands out, because a sparse gather cannot be written as the canonical fill loop the proof recognizes and nothing else may hand a raw store out as an array.
Measured with class keys at -O2, that is 301ms against 351ms for a growing list and 401ms for a walk through the pair-building iterator, so the reserved list is the best of the three available forms and still about half again the single copy the unchecked version did.
`Set.items` has no such cost because its iterator yields the element itself rather than a pair, so it fills the result directly: 132ms against 299ms for the reserved list.
The residual disappears when C4 turns `keys` and `values` into the `Iterable` views 04-collections.md specifies, which copy nothing; a key-only cursor would close it sooner at the price of code that those views delete.

A dedicated review pass over the diagnostic messages introduced across the whole migration, once the plan completes (requested 2026-08-27).

Two external handles compared with each other are accepted by sema and refused by code generation (found 2026-08-30, pre-existing).
`h1 == h2` over two handles reports `The self-hosted code generator does not support comparing 'Handle?' with 'Handle' yet`, while the spec says a handle is passed around and compared with `null`, which is the only comparison it names.
So either sema should refuse the handle-to-handle comparison and say why, or code generation should answer it by identity the way a presence check already does.

A type the statement classifier cannot read floods the statement with one problem per suffix (found 2026-08-30, pre-existing, unchanged by the suffix bound).
`atTypedVariableDeclaration` parses the type speculatively and requires `clean`, so any diagnostic other than a depth limit makes the statement not-a-declaration and drops it to the expression path, where each `?[]` reads as an empty safe subscript and reports.
Three measured shapes: a parenthesized head missing its `)` followed by 600 suffixes gives 499 problems instead of 2; a chain truncated at end of file gives 5 problems at 6 suffixes and 302 at 600; and an over-deep type-parameter bound reports only `Expected a top-level declaration` because `looksLikeFunctionDeclaration` discards the depth diagnostic on rewind and the real parse never runs.
Every message is individually true, so this is noise rather than a wrong answer, and the fix is a classifier that keeps its reading when the type is unreadable for any reason rather than only for depth.

The language server reports a spurious entry-point placement error on a single-file program (found 2026-08-30, pre-existing).
Opening `tests/inheritance.ens` says `main` may only be defined in the main module, because the server names a lone file's module after the file rather than treating the file as the program's main module, which is what `ens build <file>` does.
Folder programs and packages are both clean; only a single file is affected, so this is worth carrying to the language server's replacement rather than fixing in the one being retired.
Its parser also bounds no nesting at all, so all four deep shapes reach its stack the way they used to reach the compiler's (found 2026-08-30); the replacement needs the bound the compiler's parser now has.

## Reminders

When `@std.time` is designed, replace `Metadata.modifiedMillis` and `wait(long timeoutMillis)` with a proper duration/instant type; the names carry the unit until then.
Constructors cannot be `throws` (selfhost/sema/src/phases/members.ens:501, deliberate), which is why anything whose creation does I/O uses a static factory: `TemporaryDirectory.create()`, `TemporaryFile.create()`, `Path.open()`.
Threads are coming (outside this redesign): they unlock separate-stream reading without deadlock hazard, a possible live merged-output mode, and parallel test isolation.

## Deferred by explicit decision

`computed` properties.
Subscript declarations for user types.
`try?`, `finally`, `defer`.
Folder-facade and multi-name imports.
Compiled documentation examples.
Copy-on-write value-semantics collections.
`binarySearch`.
`Seek` on streams.
Unicode case conversion, locale collation, normalization.
Conditional conformances, as opposed to conditional members.
A `where` clause relating two type parameters.
An lstat-shaped `symlinkMetadata`.
`Environment` as an `Iterable`.
A merged-output mode for `ChildProcess`.
`TemporaryFile` variants that open the file directly.
