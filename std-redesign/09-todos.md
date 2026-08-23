# Open questions and deferred items

## Blocking decisions, needed before or during implementation

The closure spelling: type position and value position; not `function`.
The primitive binding declaration form: `primitive string implements ... { }` is a placeholder.
The error type `string.fromBytes` throws: `ParseError` was cut, `IoError` is written as a placeholder.
The `@std.text.string` file name and whether `parse` stays a separate file.
`assertEqual` on floats: tolerance form or ban.

## Level-3 passes still to write

`@std.fs`: full signatures, including symlink policy for the walk, Windows UNC and drive-relative rules, atomic write, temp files and directories as guard types, `entries()` ordering, TOCTOU guidance on `exists`.
`@std.process`: full signatures, including stdin wiring, timeouts and kill, PATH lookup rules, destructor behavior for a running child, the shell escape hatch's name.
`@std.environment`: full signatures, including whether `arguments()` includes argv[0] and the unknown-executable-path shape.
`@std.system` (internal native surface): the bridge list, errno-to-kind mapping in exactly one place.
The prelude mechanism for `print`/`eprint`.

## Implementation work items surfaced by the design

Shortest round-trip float formatting (for `StringBuilder.append(double)` and interpolation of doubles); exists nowhere today.
Number formatting beyond decimal: radix, width, padding; and parsing/formatting for `decimal`.
A modification counter in `Map`, `Set`, and their views, aborting on mutation during a walk.
The compiler's rejected-key-type list for `Map`/`Set`/`SortedMap` keys: arrays, external handles, mutable collections.
A diagnostic at instantiation when substitution nests optionals, if any call site would silently change meaning.
A test that ARC releases live locals on the throw path.
A pinning fixture for file-next-to-folder module resolution, plus a spec line.
A fixture for a cross-package subclass calling a protected constructor.
The 23 `new Error(...)` sites that break when `Error` goes abstract, most of which should throw `TestFailure`.
An `ens test` harness change so `TestFailure` moves from `@std.testing` into the error taxonomy cleanly.

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
Conditional conformances (as opposed to conditional members).
A `where` clause relating two type parameters.
