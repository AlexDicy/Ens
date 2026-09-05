# Open questions and deferred items

Every item names when it is done: a milestone of 10-migration-plan.md, a phase, or the work that follows the redesign.

## Any time before Phase D

Two consistency items the C4 and pre-C5 work surfaced, both independent of the library migration, so they wait on nothing and block nothing.
An array literal holding `this` inside a generic body (`[this, this]` for a `T[]` parameter, or `Box<T>[] xs = [this];`) is refused, because the literal's element type is the bare template and assignability equates the template with its self-instantiation only at the top level, not under `[]`; since 2026-09-04 the refusal even reads "expected 'Box<T>[]', got 'Box<T>[]'". The clean resolution is `this` carrying the self-instantiation itself, or the equation applying structurally.
A subclass method whose name a private base field uses is still refused, since `checkFieldMethodCollision` searches the flattened field list without the exemption private base fields gained on 2026-09-04; consistency would let it through, and it is a conservative refusal rather than an unsoundness.

## C8 and C9

OS-level redirection for `run(captureOutput: true)`, wait-with-timeout, and kill in the native bridges.
Every failure from the standard streams carries `ErrorKind.Other`, because telling `Closed` apart needs errno; C9's `errorKindFromErrno` lets `io.ens` and the buffered wrappers name the kind, and the message carries the detail until then.

## Phase D

The toString marker flip: after the C6 text rewrite gives `StringBuilder` its `export override toString()`, an unmarked class method named `toString` becomes an error, closing the A4 transition rule (ratified 2026-08-28).
A dedicated review pass over the diagnostic messages introduced across the whole migration (requested 2026-08-27).

## After Phase D

Emission is not reachability-based, so every function of every loaded module is lowered and linked whether or not a program can reach it (`lowerModule` in `selfhost/codegen/src/driver.ens` lowers "every function it defines", and no linker dead-strip flag is passed either).
Two costs measured, which compound: routing `print` through `@std.system` took a hello-world from 2 modules and 151,552 bytes to 12 modules and 238,592 bytes (2026-09-05), and C6 then added all of `std.text.string`, 32 KB of a hello-world's 160 KB of objects, since `lower/index.ens` pushes every bodied binding member into its module's function list.
Together they cost 21% of suite wall time and 34% of `codegencheck`'s, measured against a 177-second baseline; the bootstrap moved only 5%, so the Ens scanning members are not the cost and a fixed per-program charge is.
C9 removes the `@std.system` half by shrinking that module, but the binding half is permanent, since bindings load implicitly into every program.
Ratified 2026-09-05: carry both until after Phase D, then make emission reachability-based, rooted at the entry point plus what data can reach (vtable and interface-table slots, the `hash`/`equals`/`toString` descriptor slots, and every `export` in a library build).

A call through a function value held in a field retains and releases the closure around every call, and a function-typed parameter is retained at entry and released at exit, so a comparator handed down a recursion pays two atomics per level (measured 2026-09-03 at -O2: 2ns per call through a parameter inside one function, 16ns through a field).
Escape analysis in code generation elides both (ratified 2026-09-04 as a post-redesign pass); until then `SortedMap` reads `this.order` at every step and recurses in its lookup rather than looping, since a loop retains and releases every node it moves onto.
When `@std.time` is designed, `Metadata.modifiedMillis` and `wait(long timeoutMillis)` take a proper duration or instant type; the names carry the unit until then.

## The language server's replacement

The current C++ server is temporary; these are carried to its replacement rather than fixed in it.
It reports a spurious entry-point placement error on a single-file program, because it names a lone file's module after the file rather than treating the file as the program's main module, which is what `ens build <file>` does.
Its parser bounds no nesting, so deep shapes reach its stack; the replacement needs the bound the compiler's parser has.
It no longer checks what a lambda's body throws, because a lambda is held to the throws list of its target function type and the server does not track a lambda's target; the compiler owns the rule, so the cost is one missing diagnostic rather than a wrong one.
Three diagnostics anchor to the wrong node: the interface-widening error to the whole class declaration (`ThrowsAnalyzer.cpp:470`), "'try' is not needed here" to the call instead of the keyword (`ThrowsAnalyzer.cpp:261`), and "cannot be 'final'" to the whole method declaration; underneath, `lsp/server/DiagnosticBridge.cpp` computes a range as `startCh + length` on one line, so a multi-line node's range runs past its line.
Its type model carries no thrown-type list on a function type, so a type argument that appears only in a `throws` list can never be inferred there.
It does not treat a value of `Bag<int>`, where `Bag<T> extends Iterable<T>`, as an `Iterable<int>`, which `tests/interface_extends.ens` shows as spurious assignment and `override` errors.
Its parser rejects a local declaration whose type is a parenthesized function type with a `throws` list, `(() -> int throws Failure) safe = ...`, and misreads every statement after it in the block.
A generic static call result does not feed its type-argument inference: after `let words = List<string>.of([...])`, `countOf(words)` reports "Cannot infer type argument 'T'" while the compiler infers it.
It lacks the compiler's dedicated function-value and array-element interpolation messages and falls through to its generic "only string, integer, ..." line for those shapes.

## Reminders

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
