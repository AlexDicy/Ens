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
Ratified 2026-08-30: keep the direct rejection and make the bound honest, so the generic path refuses too, rather than granting every value an identity.
A function value's identity is not stable under the compiler's freedom to share one closure object per capture-free lambda site or to allocate a fresh one per evaluation, so equality on it would answer questions about code generation rather than about the function; a class holding the function is how a program that needs identity gets it.
The work belongs to C4, which designs the containers and their key rules.
A test that ARC releases live locals on the throw path.
A pinning fixture for file-next-to-folder module resolution, plus a spec line.
A fixture for a cross-package subclass calling a protected constructor.
The 23 `new Error(...)` sites that break when `Error` goes abstract, most of which should throw `TestFailure`.
OS-level redirection for `run(captureOutput: true)`, wait-with-timeout, and kill in the native bridges.
The toString marker flip: after the Phase B seed and the C6 text rewrite give `StringBuilder` its `export override toString()`, Phase D makes an unmarked class method named `toString` an error, closing the A4 transition rule (ratified 2026-08-28).
A ruling on `new T[n]` in generic bodies: it bypasses element-defaultability checking and is never re-checked at monomorphization, so `List<Pair>` today materializes struct values whose non-defaulted `const` fields hold zeros nobody assigned (verified 2026-08-27, pre-existing).
The naive fix, re-checking at monomorphization, would reject the collections' own backing arrays (`new T[4]` in `List`), so the containers need a sanctioned raw-storage story first.

A dedicated review pass over the diagnostic messages introduced across the whole migration, once the plan completes (requested 2026-08-27).

Two severe pre-existing compiler bugs found during the A7 review (2026-08-29), both reproducing on the pinned seed as well, neither caused by the migration.
An override that narrows a base's `T?` return to a bare `T` is accepted by the type checker, but a call through a base-typed reference reads the wrong shape: garbage at -O0 and a segmentation fault at -O2, because the override returns a bare value where the base's ABI declares the optional pair.
A `switch` arm testing a nullable type (`is Node?`) panics with an internal message about an unexpected node slot.
The `?[` safe-subscript is recognized from a question mark followed by an open bracket with no adjacency requirement, which costs two spellings: `x as? Foo?[0]` reads the cast target as `Foo?`, and a ternary whose then-branch is an array literal (`cond ? [1, 2] : [3, 4]`) cannot be written at all.
One fix covers both, the adjacency test the `??` operator already uses since A7.
`??` over a checked cast on its right operand (`fallback ?? (x as? Foo)`) fails lowering with an unbalanced-ownership message, parenthesized or not.

A ruling on bounding tree depth for chains and statement nesting (found 2026-08-30).
The A8 work bounded nesting for parenthesized expressions and types, but four shapes still exhaust the stack in the phases that walk the tree: long infix chains, postfix call chains, deeply nested blocks, and long type-suffix chains.
The first three do the same on the pinned seed, so only the suffix chain is new, and bounding them changes the diagnostics any long chain produces, which is why it needs a decision rather than a quiet limit.

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
