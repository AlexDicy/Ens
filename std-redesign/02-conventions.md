# Library-wide conventions and policies

## Failure model

Three channels, each with one meaning.
`throws` means the world said no: a missing file, a failed spawn, a denied permission.
`T?` means legitimately absent: `map.get`, `parseLong`, `readLine` at end of input.
Panic means the caller broke a contract, from a fixed closed list that never grows: index out of range, `pop`/`first`/`last` on empty, divide by zero, non-null assertion failure.
Today `List.pop()` on empty silently corrupts the count; under this rule it panics.

## Error taxonomy

One error class per module, each carrying a `kind` enum that is also per module.
The classes: `IoError`, `FileSystemError`, `ProcessError`, `TestFailure`, plus abstract `Error` in core.
Typed-throws lists stay one type long, so a new failure condition is a new enum member and no signature changes.
A per-condition class hierarchy was rejected: it is Java checked exceptions, where every new failure mode propagates a signature change to every caller.

Each module exports its own `ErrorKind`, referenced module-qualified: `fs.ErrorKind.NotFound`.
A single global enum was rejected because a switch inside `catch (FileSystemError e)` would have to account for members that module can never produce, destroying the exhaustiveness check that motivated the enum.
A kind member exists only when a program would plausibly branch on it; `Other` plus the message carries the rest.
Rust's roughly forty-member, non-exhaustive `io::ErrorKind` is the outcome to avoid.

`Error` carries `cause: Error?`, and `stackTrace()` prints the chain.
Std always declares its thrown types explicitly, never bare `throws`.
One boundary rule: stream operations throw `IoError` everywhere, and `FileSystemError` is for path-shaped operations (open, remove, metadata).
That split keeps `copy(reader, writer)` writable with one throws clause.

Message style: what failed, which value, what to do next; lowercase, no trailing period.

## Iteration

`Iterator<T>` has one method, `next() -> T?`, made safe for every element type by nested optionals.
`Iterable<T>` has `makeIterator()`.
`Collection<T> extends Iterable<T>` adds `length()`, `isEmpty()`, `contains(value)`.
The ladder is read-only; mutation lives on the concrete types.
No indexed `Sequence<T>`: nothing in std would consume it, and adding it later is not breaking.

No lazy adapter chains in std, ever: no `map`, `filter`, `take`, or `fold` pipelines.
Iteration is `for (let x in xs)`, and std ships terminal helpers that return concrete values.
This is the guardrail against a functional dialect appearing.

Closures appear at the leaves only: compare, predicate, key, factory.
Never for control flow (`fs.walk` returns an iterator, never takes a visitor).
Refinement: std may store an ordering function, and nothing else.
A comparator runs synchronously inside operations the caller asked for, so it is data; the hazard the rule targets is a callback running at a time the caller did not initiate.

## Copying

Collections have reference semantics plus an explicit `copy()` through `Copyable<T>`.
`copy()` is unconditional and shallow: a second container holding the same elements.
No `deepCopy` in std, even though conditional members could express it.
Java's `Cloneable` and C#'s `ICloneable` blessed one global protocol without specifying depth and both are regretted; a correct generic deep copy also needs cycle detection, which is a subsystem rather than a method.
Std never retains a collection a caller passed in.

## Naming

PascalCase types, camelCase members, PascalCase enum members, full words over abbreviations.
Sanctioned exceptions, written down as exceptions: `fs` and `io` as terms of art.
Verb pairs: the imperative mutates in place, the past participle returns a new value (`sort`/`sorted`, `reverse`/`reversed`).
Conversions: `toX()` copies, `asX()` is a cheap view sharing storage, `X.parse(text)` goes from text to value.
`length()` everywhere, `isEmpty()` alongside it, and `size` is never introduced: `string.length` and `array.length` are already builtins, so `size()` on containers would make the primitive and library spellings disagree.
Bool naming: `is` when the answer is an adjective about the receiver (`isEmpty`, `isAbsolute`), a plain verb when a natural one exists (`contains`, `startsWith`, `exists`), never a bare adjective.
Booleans in public API only for a genuine two-state choice read as a named argument at the call site; an enum when a third state is plausible.
Argument order: data first, options last, source before destination.

`toString()` is total: never throws, never returns null.
A conversion that can fail or lose information gets its own name (`string.fromBytes` throws, `fromBytesLossy` substitutes).

## Documentation

Doc comments state the rule, then the edge cases, in prose; `@std.path`'s voice is the model.
No design rationale in source comments.
Compiled documentation examples are deferred.

## Determinism and order

`Map` and `Set` iteration order is explicitly unspecified and documented as changeable between releases; `SortedMap` is the documented alternative.
Sort stability is deliberately unspecified.
Test rule: no test may depend on any unspecified order, tie order in sorts included.

## Resources

Every resource type has a destructor; cleanup belongs to types, not clauses.
`close()` stays explicit and throwing where losing an error would lose data (the write path), with the destructor as the backstop.
Non-resource cleanup gets a guard type, such as a temporary directory that removes itself.

## Layering

```
@std.core         Error, StackFrame, Comparable, Copyable   (implicitly imported)
@std.text         string binding, StringBuilder, parsing
@std.collections  ladder, Entry, List, Map, Set, Deque, PriorityQueue, SortedMap
@std.io           standard streams (io.ens), streams/, buffered/, text/, memory/
@std.fs           Path, File, Metadata, Entry, EntryKind, ErrorKind
@std.process      run, spawn, ChildProcess, ExitStatus, ErrorKind
@std.environment  Environment, arguments, platform, executablePath
@std.testing      assertions
@std.system       every external declaration, internal only (public, never export)
```

The prelude injects `print` and `eprint` only; `@std.core` stays implicitly imported and holds errors and the two contracts.
Per-file modules stay; imports name files.
One internal module owns every `external` declaration, so no user program can reach a libc symbol through std, and no other std module declares `external`.
Naming hazard to remember: `@std.system` is being reused for the internal native module while the current public `@std.system` still exists, so the same name means opposite things until the migration completes.
