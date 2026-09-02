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
`@std.hash` retired on 2026-08-31 and `Map` and `Set` name their key types with no bound, so there is no bound left to make honest: the judgment is the compiler's own, and it has to be made where a value is hashed and against the type argument an instantiation supplies.
Function types are the verified case (2026-08-30): `Set<(int) -> char>` compiles, and the elements hash by object identity, so two identical lambdas written at two sites are two keys while a capturing lambda evaluated twice is also two keys.
Writing `hash()` on a function value directly is already refused, so the generic path is the inconsistency.
External handles are refused by code generation as of 2026-08-30, in both the `Handle` and the `Handle?` spelling, with a message naming the type; sema still accepts the call, so moving that refusal to where it belongs is the same piece of work.
Ratified 2026-08-30: keep the direct rejection and refuse the generic path too, rather than granting every value an identity.
A function value's identity is not stable under the compiler's freedom to share one closure object per capture-free lambda site or to allocate a fresh one per evaluation, so equality on it would answer questions about code generation rather than about the function; a class holding the function is how a program that needs identity gets it.
The work belongs to C4, which designs the containers and their key rules.
Ratified 2026-09-02, as two rules: a type with no hash at all (an external handle, a function value) is refused by a per-instantiation obligation at every `hash()` call on a type parameter, the way `==` is checked today, so user generics are covered and the code-generation refusal of handles moves into sema; a type that hashes by content but can change (an array, or any collection) is refused as a key at the instantiation site, by the compiler's by-name list of the standard containers' key parameters, extended to every C4 container.
A struct key is refused when any field, transitively, holds an array or a collection, naming the field, the way the comparability walk already descends into struct fields.
A test that ARC releases live locals on the throw path.
A pinning fixture for file-next-to-folder module resolution, plus a spec line.
A fixture for a cross-package subclass calling a protected constructor.
OS-level redirection for `run(captureOutput: true)`, wait-with-timeout, and kill in the native bridges.
The toString marker flip: after the Phase B seed and the C6 text rewrite give `StringBuilder` its `export override toString()`, Phase D makes an unmarked class method named `toString` an error, closing the A4 transition rule (ratified 2026-08-28).
`Map.keys` and `Map.values` copy each element twice, once into a capacity-reserved list and once into the array they hand out, because a sparse gather cannot be written as the canonical fill loop the array-element check recognizes.
Measured with class keys at -O2 that is 301ms, against 351ms for a growing list and 401ms for a walk through the pair-building iterator, so the reserved list is the best of the three available forms; `Set.items` pays nothing because its iterator yields the element itself and fills the result directly.
C4 removes the copy entirely by turning both into the `Iterable` views 04-collections.md specifies, and a key-only cursor would close it sooner at the price of code those views delete.

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
It no longer checks what a lambda's body throws at all (2026-09-01), because a lambda is now held to the throws list of its target function type and the server does not track a lambda's target: the target arrives from a parameter, a declared local, a field, an array element, or an aggregate field, and reporting without it would flag valid code.
The compiler owns the rule, so the cost is one missing editor diagnostic rather than a wrong one, and the replacement should carry it.

Three diagnostics the server anchors to the wrong node, so the message is right and the squiggle is not (found 2026-09-01, all pre-existing, deliberately not fixed since the server is temporary).
The interface-widening error anchors to the whole class declaration instead of the implementing method's name, at `ThrowsAnalyzer.cpp:470`, where the compiler uses the method's name span.
"'try' is not needed here" anchors to the call instead of the `try` keyword, at `ThrowsAnalyzer.cpp:261`.
"cannot be 'final'" anchors to the whole method declaration instead of its name, in `Analyzer.cpp` near the private-final check.
Underneath the first of those, `lsp/server/DiagnosticBridge.cpp` computes a range as `startCh + length` on one line, so any diagnostic anchored to a multi-line node produces a range running past the end of its line; the replacement's bridge should measure the node's real end position.

Three more false errors the server shows on valid code, all pre-existing and all left for its replacement (found 2026-09-02 by the inference review, on `tests/generics_inference.ens`).
Its type model carries no thrown-type list on a function type, only a flag, so a type argument that appears only in a `throws` list can never be inferred there.
It does not treat a value of `Bag<int>`, where `Bag<T> extends Iterable<T>`, as an `Iterable<int>`, which `tests/interface_extends.ens` already showed as spurious assignment and `override` errors before this change.
Its parser rejects a local declaration whose type is a parenthesized function type with a `throws` list, `(() -> int throws Failure) safe = ...`, and every statement after it in the block is then misread.

A private base field is still treated as inherited, unlike a private base method (found 2026-08-31, pre-existing).
A subclass declaring a field whose name a private base field already uses is refused with "Field is already declared in base class", and because that ends the subclass's own field collection, its own reads of the name are rewired onto the base's private field and refused a second time for privacy, from inside the subclass.
Methods gained the opposite rule on 2026-08-31: a private base method is not inherited, so a subclass may reuse the name for a member of its own.
The field fix is more than a filter, because base fields flatten into the subclass's layout: the duplicate check has to skip private base fields, and field resolution inside the subclass has to prefer its own, which means two slots carrying one name.

An override may still be marked less visible than the method it overrides (found 2026-08-31).
`protected override greet()` on a middle class whose base declares `greet()` publicly compiles, and the method stays reachable through the base's dispatch slot, so the marker narrows who may name it and nothing else.
Virtual dispatch itself is right: a further subclass overriding the same method answers for its own instances.
What is unsettled is whether the narrowing marker should be legal at all, which is the override-visibility question the visibility ladder deliberately set aside, so it wants a ruling rather than a fix.

A nullable type argument cannot use `assertEqual` or `assertNotEqual` (pre-existing, re-surfaced 2026-09-01 by the C2e review).
The failure path interpolates the values, interpolation refuses a possibly-null value, and a generic body has no way to peel one level off an arbitrary `T`, so `assertEqual<string?>` is refused per instantiation and a test over an optional result unwraps by hand first.
The clean fix is a language ruling, not a library one: whether an interpolation hole accepts a single-level nullable and prints `null` for the absent case, the way an array element already does inside the array text form.

Type-argument inference binds a parameter only where it appears directly as a parameter type or as an array's element type, never inside an instantiation (re-surfaced 2026-09-02 by the C3b std tests).
`assertExhausted<T>(Iterator<T> walk)` called with an `Iterator<string>`, and `firstOf<T>(List<T> items)` called with a `List<string>`, both report `Cannot infer type argument 'T'`, so the caller writes the argument out.
The spec states the direct-position rule, so this is a design limit rather than a bug, and C4's free functions over `Iterable<T>` will meet it; unifying the argument's instantiation with the parameter's is the natural extension and wants a ruling.
Ratified 2026-09-02: inference unifies the argument's type with the parameter's through instantiations, nested, and through implemented interfaces and base classes, so a class implementing `Iterable<int>` binds `T` for an `Iterable<T>` parameter; it lands as its own sema-only milestone before C4.

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
