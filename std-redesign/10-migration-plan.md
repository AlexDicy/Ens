# Migration plan

How the tree moves from today's `libs/std` to the design in this folder without ever breaking the build.

## The constraint that shapes everything

The seed pinned in `seed.json` compiles `libs/std` and the selfhost sources when it builds stage1.
So neither std nor the compiler's own sources may use a new language feature until a released seed supports it.
The order is therefore forced: implement every language feature first, in sources written in today's syntax; cut a seed release; only then rewrite std using the new features; only then migrate consumers and delete the old modules.

The gate for every milestone is the existing one: the full suite plus the bootstrap fixpoint, stage2 byte-identical to stage3.
One commit per milestone.

## Phase 0: verifications before anything depends on them

- V1: a test that ARC releases live locals on the throw path; the whole cleanup story rests on destructors.
- V2: a fixture pinning file-next-to-folder module resolution (`io.ens` next to `io/`), plus a spec line.
- V3: a fixture for a cross-package subclass calling a `protected constructor`.

## Phase A: language features, one milestone each

Sources stay in today's syntax throughout; each feature lands with sema, codegen where it has one, spec text, and tests.
The order puts independent small features first and the two big ones last, so a stall on the big ones delays nothing else.

- A1: `const` on fields; write-once enforcement in definite assignment, struct literals included.
- A2: interfaces extending interfaces; conformance and monomorphization follow the flattened set.
- A3: `static` members, reachable only through the type name.
- A4: `toString` overridable on classes; dispatch already exists through the descriptor slots.
- A5: class-typed generic bounds (`E: Error`); bounds today name interfaces only.
- A6: conditional members: the constraint-vs-declaration rules for member type-parameter lists, monomorphization filtering, and the unmet-bound diagnostic.
- A7: nested optionals: remove the collapse, audit the six places that assume one level, rewrite the two tests that assert collapsing, add the spec section for the level rules.
  This is the one Phase A feature that can change the meaning of existing code, so the audit includes every generic instantiation in the tree whose argument is already nullable.
- A8: closures: the type form, the value form, by-value capture, throws inference, EIR lowering, ARC for captured references.
  The spelling is signed off: inferred-parameter arrow lambdas, explicit types legal, all-or-nothing per lambda.
- A9: primitive binding declarations: the declaration form, member lookup routing, go-to-definition; the intrinsic core stays in the compiler.
- A10: the prelude mechanism for `print` and `eprint`, alongside the implicit `@std.core` import.

The temporary C++ LSP front end gets parse-level support per feature so the editor shows no false errors, and no more; it is not a language authority and it is scheduled for replacement.

## Phase B: seed release

- B0: a declaration whose name and parameter types match a function the language provides replaces it in the declaring module instead of colliding with it, so the library can take over what the compiler provided.
  The seed has to carry this rule before C5 and C6 can land as ordinary commits: each of them adds a declaration in a commit the preceding seed has to compile.
- B1: cut and publish the seed that supports A1 through A10 and B0, and pin it in `seed.json`.
From here on, std and selfhost sources may use every new feature.

## Phase C: the new std, module by module, old kept alive

New modules land next to the old ones, and a consumer moves when its area lands; nothing is deleted until Phase D.
Within each milestone the std change and its consumer updates are one commit, so the tree never holds a half-move.

- C1: `@std.core` rewritten: abstract `Error` with `const` fields and `cause`, `StackFrame` as a struct, `Comparable`, `Copyable`.
  The same commit fixes the 23 `new Error(...)` sites, most of which become `TestFailure`, and retires `@std.hash`.
  `StackFrame` needs a seed release of its own, found while implementing it: the seed compiles this tree's `libs/std`, and the trace runtime it carries builds every frame as a class object with a type descriptor, which a struct has none of.
  So the flip is three steps, since the seed used across it has to read both shapes: the trace runtime learns to build frames from whichever shape the library declares, that seed is published, and `core.ens` flips after it.
- C2: `@std.testing` rewritten on the new `Error`, including `assertThrows` and structured diffs.
  Writing the surface 08-testing.md specifies needs four language changes first, each its own commit, because a library may only use what the seed already compiles: an explicit `throws` list legal wherever `throws` is written, a function type able to carry one, a type parameter allowed where a class type is required for a runtime type test, and an array comparing and formatting by its contents.
  A seed release then follows, and the library commit lands last, so the order is C2a through C2d, the seed, and C2e.
- C3: the iterator flip, one coordinated commit: `Iterator.next() -> T?`, the `for`-in lowering, every iterator in std, and every hand-written iterator elsewhere in the tree.
  This is the one cross-cutting break that cannot be staged, which is why it comes before the container rewrite rather than with it.
  The seed constraint splits it the way it split `StackFrame`: the pinned seed lowers every `for`-in loop in the compiler's own sources against this tree's `libs/std`, and its lowering reads only the `hasNext` shape, so the library cannot flip until a seed reads the new one.
  So C3a teaches the lowering to drive whichever shape the library's `Iterator<T>` contract declares, a seed release follows, and C3b flips the interface, every iterator, the fixtures, and the spec in one commit and deletes the gated branch.
- C4: `@std.collections` rewritten: the ladder, `Entry`, the six containers, conditional `sort`, `getOrInsert`, views, the modification counter.
  `Pair` dies here; consumers of `Map` iteration move to `Entry`.
  Before it, one sema-only language milestone: type-argument inference through instantiations and conformance (ratified 2026-09-02), which needs no seed because the library does not depend on it.
  The `new T[n]` question the plan carried is closed: the array-fill check already runs per instantiation, and the containers' backing stores use the std-only `RawArray<T>`, so no ruling is needed.
  Split into three commits: C4a, the compiler's key-type and hash rules; C4b, `List`, `Map`, and `Set` rewritten with `Entry`, `Collection`, the views, the modification counter, `removeWhere`, and every consumer, deleting `Pair` and the sorting module; C4c, `Deque`, `PriorityQueue`, and `SortedMap`.
  C4b surfaced two compiler bugs the seed has to carry before the library can land: `this` inside a generic body was typed as the bare template, so a container could not hand itself to its iterator, and the synthesized array-content helpers were named without their module, so two modules comparing the same array type collided at link time.
  Both are fixed in their own commits and a seed release sits between them and the library.
- C5: `@std.io` written from scratch: `io.ens` plus the four submodules; `print`/`eprint` rerouted through the prelude onto `io.out()`.
- C6: `@std.text` rewritten: the `string` binding replaces `@std.text.strings`, `StringBuilder` loses `appendByte`, `parse` lands; consumers move from `strings.split(text, sep)` to `text.split(sep)`.
- C7: `@std.fs` written: `Path` absorbs `@std.path` as methods, `File` moves onto the stream contracts, metadata, entries, walk, temp guards.
- C8: `@std.process` and `@std.environment` written: `run`/`runShell`/`spawn`, `ExitStatus`, `Environment`; the old `run`/`runCaptured`/`start` family keeps working until D1.
- C9: the internal `@std.system` native module: every `external` declaration moves in, `errorKindFromErrno` lands, and the old bridges are aliased from it so C7 and C8 could build on it retroactively if ordering demands.

Consumer migration inside C means the selfhost compiler, build, cli, and lsp sources plus `tests/` fixtures, area by area, in the same commits as their std milestone.

## Phase D: deletion and the record

- D1: delete the old public `@std.system`, `@std.path`, `@std.text.strings`, `LineBuffer`, and the old process overload family; the name `@std.system` now means only the internal native module.
- D2: rewrite the std chapters of `spec.md` to describe the new library, honoring the spec-scope rule: user-facing behavior only.
- D3: mark this folder's documents as implemented, moving anything still open into the issue tracker or the TODO file.
- D4: cut the release whose seed makes the new std the one every consumer builds against.

## Standing rules for every phase

Any agent brief for a milestone states the quality bar explicitly: HIGH, parallel-ready, clean code, and the milestone's gate.
A compiler bug found mid-milestone is surfaced and fixed, never designed around.
Every grammar spelling the plan needs is signed off; a new one arising mid-milestone stops for sign-off first.
