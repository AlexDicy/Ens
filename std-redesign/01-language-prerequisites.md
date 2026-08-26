# Language prerequisites

Every feature here was decided during the redesign and must exist before the library is implemented.
Each was signed off on the concrete shape described.

## Function values (closures)

Adopted, with by-value capture only.
A lambda copies what it captures and cannot mutate an enclosing local.
This avoids boxed upvalues, aliasing surprises, and mutable-capture escape analysis.
A cycle is possible only by storing a closure inside an object it captured, which `weak` already covers and std never does.

The spelling: type position is `(T, T) -> int`; value position is an arrow lambda with parameter types inferred from the target type, `(a, b) -> a.age - b.age`, with a block body written `(a, b) -> { ... }`.
Explicit parameter types stay legal, and one lambda is all typed or all untyped, never mixed.
The keyword `function` is not used and not reserved.

## Interfaces extending interfaces

`interface Collection<T> extends Iterable<T>` becomes legal, reusing `ExtendsClause` on `InterfaceDeclaration`.
Required for the collections protocol ladder.

## `static` members

Implemented, reachable only through the type name: `List.withCapacity(8)` is legal and `myList.withCapacity(8)` is an error.
This is the C# rule; Java allowing instance access is the known mistake.

## Nested optionals

The silent collapse of `T??` to `T?` is removed.
The collapse lives in `selfhost/sema/src/model/types.ens:958-973` and, for editor diagnostics, `lsp/frontend/semantic/TypeContext.cpp:23-31`.

Layout: a tagged pair only when nested.
`string?` stays a bare pointer and `int?` stays `{i1, i32}`, so nothing existing gets bigger or slower.
Codegen already handles the nested form, since an optional inner type is not a reference type.

User-facing rules:
`null` always means the outermost level.
Assigning a `T?` to a `T??` wraps once; assigning a `T` wraps twice.
`x == null` tests only the outer level, and narrowing takes `T??` to `T?`.
`??` unwraps exactly one level.
`?.` and `?[` on a doubly-nullable value are errors that say to unwrap first.
`as?` and `is` keep rejecting nullable targets.

Rationale: substitution producing `T?` where `T` is already nullable currently truncates silently, so an `Iterator<string?>` stops at the first null element.
Nesting turns that into a compile error at the use site.

## `const` on fields

`export const string message;` becomes legal: write-once, assigned by the constructor or a struct literal, readable everywhere, assignable nowhere else.
`spec.md:193` currently restricts `const` to locals.
Needed because `Error`, `StackFrame`, `Metadata`, `ExitStatus`, and `Entry` are immutable data whose fields are otherwise writable from outside.
A private field plus an accessor was rejected because a field and a method cannot share a name.

## `toString` on classes

Classes may override `toString` the way structs already do.
`spec.md` currently lists `toString` as overridable for structs only.
`toString()` is total: it never throws and never returns null.
Any conversion that can fail or lose information gets its own name and signature.

## Conditional members

A member may constrain the enclosing type's parameters by restating them with a bound in its own type-parameter list: `export sort<T: Comparable<T>>();`.
Zero grammar change: `TypeParameterList` already parses in that position.

Two rules give the form meaning.
A name matching an enclosing type parameter is a constraint on it, not a declaration, and must carry a bound; a bare re-used name is an error, which makes renames detectable.
A list may not mix constraints and declarations.

Semantics: the member exists only for instantiations that satisfy the constraint.
Calling it elsewhere is an error naming the parameter and the unmet bound.
Applies to methods, statics, and constructors; constructors are the valuable case.
A conditional member may not be `override` and may not satisfy an interface requirement.
Monomorphization emits only satisfying instantiations, so there is no runtime cost.
Completion must not offer a conditional member on a non-satisfying instantiation.

Known limit, accepted: the form cannot relate two different parameters, such as `K: Comparable<V>`.
Nothing in std needs that, and a `where` clause can be added later without conflict.

## Primitive binding declarations

A std module written in Ens declares a primitive's members; the compiler links the module to the primitive.
`"text".split(",")` then works like a builtin, and go-to-definition lands in readable Ens source.
The declaration is `primitive string implements ... { }`, with `primitive` a contextual keyword legal only in std; `this` inside is the primitive value, statics are allowed, and the intrinsic core is not declared in the binding because the compiler owns it.

A binding can declare interface conformances, which is how primitives participate in generics: `primitive string implements Comparable<string>`.
Numeric primitives get `Comparable` the same way, which is what makes `SortedMap<long, V>` work with no special case.
`string` does not implement `Iterable<char>`: iteration is explicit through `chars()` and `bytes()`, and `for (let c in text)` does not compile.

The intrinsic core is minimal: `length`, byte access, unchecked slice-to-string, `+`, `==`.
Everything else, meaning `indexOf`, `startsWith`, `trim`, `substring`, `compareTo`, `split`, `lines`, and case conversion, is Ens code over those.

## Module resolution: file next to folder

`io.ens` next to `io/` works today; `selfhost/codegen/tests/support.ens` coexists with `support/`.
The standard streams design relies on it: `@std.io` is the file, `@std.io.streams` is a file in the folder.
This needs a pinning test fixture and a line in the spec, since it currently works by construction rather than by promise.

## Considered and declined

`computed` properties: deferred; builtins keep `.length`, Ens-defined types use methods, so everything on an Ens-defined type is a method call.
Subscript for user types: declined; `list.get(i)` and `list.set(i, v)` stay.
`finally`, in any position: declined; function-level catch and finally blocks are siblings of the body and cannot see body locals, so a finally clause cannot clean up the resources it would exist for.
Cleanup belongs to types: destructors, plus guard types for non-resource cleanup.
`defer`: declined for the same reason from the other direction.
`try?`: declined; one spelling for calling a throwing function.
Folder-facade imports and multi-name imports: declined for now; per-file modules stay.
Removing free functions or namespace imports: declined; std is designed to barely need free functions instead.
Making `export` type-level only with `public` members: rejected; 558 members in `selfhost` are `public` inside an `export` type and would widen to public API, and the package-internal rung is the one Rust (`pub(crate)`), C# (`internal`), and Swift (`package`, added in 5.9) all needed.
Visibility inheritance for `override` members was considered (option C) and set aside; the ladder stays exactly as it is.

## Verification notes

ARC must release live locals on the throw path, since cleanup now rests entirely on destructors.
This is believed true, because a throw is a return on a second channel, but it deserves a dedicated test before the library relies on it.

Making `Error` abstract breaks 23 existing sites: 13 across 12 fixtures in `tests/`, and 10 embedded in `selfhost/` unit tests.
Accepted: most of those sites are the lazy-default pattern the ruling exists to prevent, and most should throw `TestFailure`.

A `protected constructor` on an exported class is reachable from cross-package subclasses.
Verified empirically and in the sema; `spec.md:366` states it as a guarantee.
Caveat: an open exported class holds protected member signatures to an export-grade floor, so parameters added to `Error`'s constructor must be exported types.
The protected-constructor case had no test fixture before this investigation; one should be added.

## Class-typed generic bounds

A bound may name a class: `<E: Error>` accepts `Error` or any subclass.
At most one class per bound list, mirroring single inheritance, combinable with interfaces via `+`.
Naming a `final` class is an error, since only the class itself could satisfy it.
