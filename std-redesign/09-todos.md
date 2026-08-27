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
The compiler's rejected-key-type list for map and set keys: arrays, external handles, mutable collections.
A diagnostic at instantiation when substitution nests optionals, if any call site would silently change meaning.
A test that ARC releases live locals on the throw path.
A pinning fixture for file-next-to-folder module resolution, plus a spec line.
A fixture for a cross-package subclass calling a protected constructor.
The 23 `new Error(...)` sites that break when `Error` goes abstract, most of which should throw `TestFailure`.
OS-level redirection for `run(captureOutput: true)`, wait-with-timeout, and kill in the native bridges.
A ruling on `new T[n]` in generic bodies: it bypasses element-defaultability checking and is never re-checked at monomorphization, so `List<Pair>` today materializes struct values whose non-defaulted `const` fields hold zeros nobody assigned (verified 2026-08-27, pre-existing).
The naive fix, re-checking at monomorphization, would reject the collections' own backing arrays (`new T[4]` in `List`), so the containers need a sanctioned raw-storage story first.

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
