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
  Its transition rule, which let an unmarked class `toString` pass as an ordinary method, closed in Phase D once `StringBuilder` carried the marker: a class method with the text form's shape now has to write `override`, and one whose shape differs is still an ordinary method.
  Ruled 2026-09-18, one rule for both kinds: on a class or a struct, a `toString` taking no parameters is reserved for the text form and must be `toString() -> string` with no `throws` and the `override` marker, while a `toString` taking parameters is an ordinary method.
  That loosened the struct rule, which had refused every `toString` the text form's shape did not fit, and it widened the class rule, which had judged only the marked ones.
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
  C4a's exclusion list grew on 2026-09-18, when `float` and `double` joined the types a `Map` or a `Set` cannot key by, beside arrays, collections, external handles and function values.
  A NaN is equal to no value, so nothing could find it again, and negative zero is equal to zero but hashes differently, so one key would become two entries; the refusal points at `SortedMap`, which keys by the order `Comparable` gives rather than by a hash.
  A struct whose fields hold one at any depth follows from the same walk, unless it declares its own `equals` and `hash`.
  Reviewing that work found the compiler's own floating-point literals were not correctly rounded, and fixing it on 2026-09-19 replaced the scaling loop with a correctly rounded conversion in `@std.text.parse` that both the compiler and `parseDouble` now read, dropping the `atof` bridge with its last caller.
  The old loop was one unit out on 230 of the 308 positive powers of ten and on 85 of 320 negative ones, on Planck's constant and on the elementary charge, and it had already made `parseDouble` refuse the largest value a `double` holds, because the library's own bound was written as a literal.
- C5: `@std.io` written from scratch: `io.ens` plus the four submodules; `print`/`eprint` rerouted through the prelude onto `io.out()`.
  The two prelude functions live in `@std.io.print` (ratified 2026-09-03), since the prelude lends every export of a listed module; the compiler stops seeding `print` as a builtin and lists `std.io.print` as implicitly imported.
  C5 needs two seeds, which the plan first missed by reasoning only about `print`.
  The streams need `lazy const`, because `io.in()` answers one shared reader, and they need the three standard-handle bridges, because the seed compiles `libs/std/src/system.ens` when it builds the compiler and would emit a reference to a runtime symbol it cannot synthesize.
  So the order is the streams, the `lazy const` feature and its seed, the bridges in the compiler and their seed, then `io.ens` with the library bridges, then the prelude flip.
- C6: `@std.text` rewritten: the `string` binding replaces `@std.text.strings`, `StringBuilder` loses `appendByte`, `parse` lands; consumers move from `strings.split(text, sep)` to `text.split(sep)`.
- C7: `@std.fs` and `@std.environment` written: `Path` absorbs `@std.path` as methods, `File` moves onto the stream contracts, metadata, entries, walk, the temporary guards, and the environment snapshot.
  `@std.environment` moves here from C8, because `Path.absolute()` reads `currentDirectory()`, `fromNative` takes a `Platform`, and the temporary guards find their root among the environment variables.
  Two prerequisites land before the library, and one seed carries both.
  C7a: a struct may declare `implements`, naming any interface, checked at the declaration, satisfying a generic bound as a direct call and never carrying an interface value, which is the rule primitive bindings already live under (ratified 2026-09-06).
  C7b: the native bridges the module needs, since `ens_path_kind` answers a kind alone: metadata carrying length and modification time, a kind that does not follow a symbolic link, `realPath`, `copyTo`, and the mapping from a platform's own error number to an `ErrorKind`.
  That mapping moves here from C9, because `FileSystemError.kind` has to tell `NotFound` from `PermissionDenied`, `DirectoryNotEmpty` and the rest, and nothing but the platform's error number does.
  It is named `errorKindFromCode`, because a Win32 error is not an errno, and its table is a pure function of the target triple so every platform's mapping is unit-tested from any host.
  A Linux target reads metadata through the raw `statx` system call (ratified 2026-09-06), since one kernel-defined layout serves every architecture and every C library, and a wrong system-call number fails loudly where a wrong field offset would not; macOS keeps libc `stat`, whose layout is documented and stable.
  Three corrections the design pass made: copying a file is an Ens loop over the streams with only the permission bits bridged, there is no bridge for the system's own error text since it is localized and a fixture could then not pin the module's messages, and a bridge answers through `out` parameters rather than an array, which spares `exists()` an allocation.
  Two bridges the plan had missed: a wide current-directory call, since the narrow one loses a directory outside the system code page and `Path.absolute()` reads it, and an exclusive-create open mode, without which `TemporaryFile.create()` cannot claim a name.
  C7c: the two modules and every consumer in one commit, with `Path` carried through the compiler's own signatures rather than built at each call, so the compiler uses the type it ships.
  Four rulings shaped it, all of 2026-09-06.
  `Path` is a struct whose text field carries a default of `""`, since a `const` field with no default would make `Path[]` and `List<Path>.toArray()` illegal, and the empty path is a real value the surface already names.
  `Environment` gains `platform()`, because a set that matches names by a platform's rule can say which rule, and every holder would otherwise thread a `Platform` alongside it.
  `TemporaryDirectory.create` and `TemporaryFile.create` take a prefix, so a temporary directory left behind by a crash says what made it.
  `@std.path` becomes a delegating shim over `Path`, while the old `@std.system`'s file calls stay as they are and die at D1, since they carry no rule that could drift.
  Three more were settled while the modules were written, also 2026-09-06.
  `entries()` and `walk()` answer `Iterable<Entry>` rather than the `Iterator<Entry>` the surface document wrote, since a for-in loop is nominal on `Iterable` and `Iterator.next()` carries no `throws`, which makes both calls eager whichever type they answer.
  The thirteen native declarations the two modules need live in `@std.system` behind thin wrappers, chosen over declaring each one in the `@std.fs` file that calls it; the per-program cost that choice accepts is measured in 09-todos.md.
  `realPath` resolves the way the operating system resolves and never normalizes the text first, so every part of the path has to be there, because a `..` written after a symbolic link names a different place than removing it from the text would.
- C8: `@std.process` written: `run`/`runShell`/`spawn`, `ExitStatus`, `ChildProcess`; the old `run`/`runCaptured`/`start` family keeps working until D1.
  Before it, one language milestone: import aliasing with `as` (ratified 2026-09-08), `import @library.rendering as lib;` and `import ErrorKind as IoErrorKind from @std.io.streams;`.
  A file that raises both an `IoError` and a `ProcessError` needs two enums named `ErrorKind`, and today two same-named imports, or two modules sharing a last segment, cannot coexist in one file, while a function is reachable only through its module's alias, so a collision has no escape but moving code to another file.
  Qualified member access, `module.Type.member`, stays out for now even though `module.Type` already resolves as a type.
  A seed release follows, since the library uses the spelling.
  Its own native bridges land with it, capture through pipes, wait with a timeout, and kill, since a bridge written before the library that uses it is a bridge written blind.
  Four rulings of 2026-09-08 shape the bridges: the runtime ignores SIGPIPE at startup so a closed pipe is an `IoError` rather than a silent death that skips every destructor; `kill()` passes 137 on Windows and the status reports `signal 9` only when that kill is what ended the child; `run` finds executables only, never a script and never in the current directory; and the design assumes the threads that are coming, so no bridge holds process-global state.
  They land in the same seed as import aliasing, one release rather than two.
  `Environment.set` panics on an empty name or a name containing `=`, since neither can cross an environment block and the ratified signature has no `throws`; a programmer error, like an index out of range (ruled 2026-09-10).
  The helpers the old `@std.system` still imports from `process.ens` move to `process/native.ens`, so the old family keeps compiling until D1, and every consumer of that family moves in the same commit, as C7c did for `Path`.
- C9: the internal `@std.system` native module, which C7c and C8 built as they went, so that by 2026-09-16 every `external` declaration already lived in that file and the milestone as written had nothing left.
  Its one measurable finding was a regression C8 introduced: `@std.system` had come to import `@std.process.native`, which imports `Environment`, `Platform` and `Path`, so every program loaded `@std.environment` and all of `@std.fs`, and a hello world had grown from 14 modules to 26; splitting `@std.process.blocks` out by dependency put it back at 14 (673a29d) and took between 94 and 157 seconds off a suite that had been running 429 to 480.
  What remains of C9's purpose, the one place for the error-number mapping, is the error-kind milestone in two halves around a seed.
  The preparation that needs no seed landed first: internal names for what the library reads from the system (53f16ba) and selfhost moved off the old `writeError` and `flush` (036a6d6).
  The seed half is seven bridges: `ens_stream_write`, `ens_stream_read`, `ens_stream_flush`, `ens_stream_close` and `ens_io_error_kind` (12546a4), each reading errno on its failure branch alone because the Microsoft runtime leaves errno sticky across a call that succeeds; `ens_fs_errno_kind`, ruled 2026-09-17, because a failed write inside `Path.writeBytes` reports an errno on every target while the Windows file-system table is Win32, so without an errno table of its own a full disk on Windows would answer `Other`; and `ens_sleep_millis`, for the wait `scratch.discard` takes between attempts when a scanner still holds a program the run just built.
  The library half follows the seed: `nativeError` on `IoError`, `FileSystemError` and `ProcessError`, the io and fs modules naming `Closed` and `Interrupted`, `@std.fs`'s write paths classifying a stream failure through `errorKindFromErrno` so `NoSpace` is reachable on every target, `Thread.sleep` in `@std.thread`, and the retry in `scratch.discard`.
  `@std.process` was expected to fold onto the classifier too and does not, for the same reason `ens_fs_errno_kind` had to exist: a process bridge answers the platform's own number, so on Windows a pipe nobody reads arrives as `ERROR_BROKEN_PIPE` or `ERROR_NO_DATA` rather than as `EPIPE`, and the errno table would answer `Other` for both.
  It keeps `meansPipeClosed` and classifies its own numbering in one place of its own, which is the only condition it can name; naming the rest would need a bridge over that numbering and so another seed.
  On Windows `FileSystemError.nativeError` is a Win32 number for a file-system call and an errno for a failure a stream call reported, since the stream layer there is the C runtime; the field's documentation says so.

Consumer migration inside C means the selfhost compiler, build, cli, and lsp sources plus `tests/` fixtures, area by area, in the same commits as their std milestone.

## Phase D: deletion and the record

- D1: the deletion, in three commits, because one commit could not stay green at every step and a diff of that size could not be reviewed.
  D1a moved the eleven fixtures that outlive the old surface onto `@std.fs`, `@std.environment` and `@std.process`, and renamed the two that test what survives to `tests/std_system_start_stream.ens` and `tests/std_system_start_timeout.ens` (8787cb4).
  Its one finding was that `fs_bridges` measures the bridge's own open against a control opened by the C library, so the control had to stay the library's open and now lives in the fixture rather than being borrowed from an export that was about to go.
  D1b deleted the old public surface: 28 exports with their six private helpers, `@std.path`, `@std.text.strings`, twelve libc declarations, the six bridges whose last caller went with them, and twelve fixtures.
  What survives in `@std.system` is exactly what `selfhost/packages/src/tools.ens` reaches across a package boundary until threads land: `start` in both overloads, `ChildProcess` and `SystemError`, and behind them `inherited`, `childVariables`, `startedChild`, the six child bridges, `LineBuffer` from `@std.text.lines`, and `@std.process.blocks`.
  So `LineBuffer` does not go at D1 after all, and the old `platform() -> string` became a private helper over `platformCode()`, which leaves those three as the file's only exported names.
  D1c removed the compiler's half: `emit/processes.ens` and `emit/spawnwindows.ens` whole, the `pathKind` and `createDirectory` halves of `emit/filesystem.ens`, the listing half of `emit/directories.ens`, the spawn half of `emit/spawnposix.ens`, `Runtime.writeError()`, and the six `resolveTarget` cases.
  Two files the survey had expected to go whole did not, because surviving bridges reach into them: `emit/directories.ens` still holds the entry points a folder is opened and closed through for `ens_fs_open_directory`, and `emit/spawnposix.ens` still holds the wait, the status and the block vectors that both child families share.
  Nothing in `emit/nativeshapes.ens` or `emit/errorkinds.ens` became dead, since neither deleted emitter imported either of them and every shape there still serves a bridge that stays.
  Two emitter tests proved properties that outlive their vehicle and moved rather than went: the define-once property onto `ens_fs_create_directory`, and the literal-cache tests onto the C-string lift, which is the surviving primitive that consumes a payload pointer.
- D2: the std chapters of `spec.md` rewritten for the library as it stands, landed 2026-09-18 (2890f71), user-facing behavior only per the spec-scope rule.
- The diagnostics review's location item, ruled and landed 2026-09-19: a judgment a generic body defers to its type arguments is reported where those arguments were written, not inside the generic.
  All six kinds were reported at the generic's own line under an `In 'Slots<Path>': ` prefix, which pointed a reader into a library they had only used: an array element that needs a default value, a hash, an interpolated value, a type test, a struct `==`, and a keyed container's key.
  Each now lands on the mention or the call that supplied the arguments and names the instantiation written there, with the generic's own line beside it as a `note:` line naming that generic, which a `RelatedLocation` on `FileDiagnostic` holds and both formatters print inside the entry the error already occupied, so a note counts as no problem of its own.
  Every one of those messages was rewritten for the reader who chose the type argument, which for most of the kinds is a second wording beside the one the line that wrote the code reads.
  A cascade hands that origin down unchanged, so a judgment reached through any depth of library generics still lands on a line that reader wrote and names only what they wrote, with the innermost generic in the note; the generics in between are not shown, which 09-todos.md records.
  The fixtures pin the new note lines through the `@expect-note` directive this added.
- The floating-point bit toolkit lands in two commits with a seed cut between them, because nothing in Ens can read a double's bits and std can only call a bridge once the pinned seed knows it exists.
  The first commit, landed 2026-09-19, is the compiler half plus the predicates that need no bridge: `emit/bitpatterns.ens` emits `ens_double_bits`, `ens_double_from_bits`, `ens_float_bits` and `ens_float_from_bits`, each one same-width bitcast wrapped in a function defined once per module, and `@std.text.numbers` gains `isNaN`, `isFinite` and `isInfinite` on `float` and `double`, the first by a value's inequality with itself and the other two by magnitude against the largest finite double.
  `tests/bits_bridges` declares the four bridges itself, in an `external from libc` block, because nothing in the standard library reaches them yet.
  The second commit, landed 2026-09-19, adds `toBits`, `fromBits` and `toCanonicalBits` to `float` and `double` over those bridges, which are declared in `@std.system` like every other bridge and reached through its wrappers, and rewrites `isNegativeZero` on the bits.
  A third commit, 2026-09-19, moved the four declarations out of `@std.text.numbers` into `@std.system`, where the one-place rule for `external` declarations puts them.
  The four diagnostics that refuse a float as a hashed key show the pair a struct with such a field declares instead of pointing at an exact value the number came from, comparing each floating-point field with `compareTo(...) == 0` and hashing it with `toCanonicalBits() as long`, so a NaN matches a NaN and the two zeros stay two keys.
  A bare float key keeps the `SortedMap` suggestion ahead of that pair, and a bare `hash()` call is sent to `toCanonicalBits() as long` directly.
  Five fixture pins moved and none was weakened, `tests/float_keys.ens` keys a `Map` and a `Set` by a struct of three doubles and one of two floats, and eight library tests and two sema tests came with them.
- The diagnostics review's naming item, ruled 2026-09-08 and landed 2026-09-19: every sema diagnostic spells a type the way the file it is reported in can name it, so a fix it suggests compiles where it is read.
  The link phase reads each module's imports backwards into a `TypeNames` its link table owns, and `LinkResult` carries one per file for the passes that report outside the file they are reading, which is the obligation pass and the instantiation-overflow refusal.
  `displayAgainst` went with it: two types that spelled alike were qualified by their modules only when they stood beside each other, and a name a file bound already tells them apart wherever they appear.
  No message changed a word beyond the type spellings, which moved four unit-test assertions and no fixture pin, since every fixture that names a type across a module boundary reaches it by a named import.
- The diagnostics review's standard-library item, the last of the review pass, landed 2026-09-19: every message the library raises, panics with, or hands a failing assertion was read against the message rubric in 02-conventions.md, over 87 text-bearing sites composing 102 texts, and 90 of them were left exactly as they stood.
  Two were untrue for a case they covered, and both were verified by running them rather than reasoned from the code: `@std.io.buffered` reported the offset inside the line or the bufferful it had just decoded as though it were an offset into the stream, and advised a `readAll()` that the consumed bytes make impossible, and `ChildProcess.take` called both of a child's output streams "the output".
  A create now words an absence as the directory the new file would go in rather than as the file itself, and `File.created`, `appended`, `claim` and `opened` refuse the empty path before asking the system, the first three as a create and the last as an open, the way `createDirectoryExclusive` and a start with no program name already refuse one.
  `Path.readBytes`, `writeBytes` and `copyTo` name their reason the way every other failure over a path does, through `failedFromErrno` and `failedBetweenFromErrno` in `@std.fs.error`, which read the errno table and carry the stream failure as the cause, so a full file system says so rather than saying only that a write failed.
  The rest were real names and consistency: `append to` over `open for appending`, `an empty priority queue` over `an empty queue`, an article on the three walk panics, the three variables a temporary location is looked for in named, the library's one `;` advice clause brought onto the `.` shape the compiler's own diagnostics use, and `Ask again` added to the one stream reason a caller can act on.
  Six fixture pins and two unit-test pins moved and none was weakened, and four tests were added, two of them pinning conditions that cannot be staged by building the failure the way the operation builds it.
  The five `SystemError` messages stayed as they are, with the module that goes when `selfhost/packages` moves to the threaded reader.
- D3: this folder's documents marked as implemented, landed 2026-09-19: the README now says the design is implemented through this phase, and that the code is the truth wherever a chapter and `libs/std` disagree.
  Every signature block in 03 through 08 was read against the module it describes, along with the prose that names a file or a member, which found 23 differences: 21 corrected here, one left as it stands, a private lowering hook the surface list omits, and one closed in the code instead, the four floating-point bit bridges moving into `@std.system` so the one-place rule the chapters state holds again.
  Most were chapters trailing decisions this phase landed, `entries()` and `walk()` answering `Iterable`, the prefix both temporary factories take, the one defaulted-capacity constructor each buffered stream carries, `final` on the two trace methods, `override` on `StringBuilder.toString`, `Environment.platform()` and the `@std.process.blocks` split, while five were spellings the compiler refuses outright, a `private` field marker and the `this.message` shorthand in the four error subclasses.
  The pointers into `spec.md`, the sema and the language server had rotted onto unrelated lines and now name what they mean, 02 gained the `EncodingError` its taxonomy had left out and lost a migration hazard that is spent, and 09-todos.md lost the two items the process bridges and the error-kind milestone had already resolved.
- The array-literal ownership defect 09-todos.md carried, fixed 2026-09-19 before the release: a body storing one borrowed reference into two places, `[this, this]` among them, was refused with `Internal: lowering produced unbalanced ownership`, because the value-ownership verifier left a borrowed temp that had handed its retained reference on with nothing to use again.
  Lowering was right and code generation did not change, so the fix is the verifier reading a retained borrow as a borrow again once that reference is given up, which accepts two locals, two fields, two closures and a store beside a return of `this` as well.
  Making a state depend on the one before it also needed the two states that answer nothing about a reference to pass through every transition, because the pass reads blocks in the order they were numbered while a comparison numbers the block it jumps to on a difference before the blocks that jump into it, which left it settling on a conflict no path produces in `tests/struct_equality.ens` and `tests/nested_optionals.ens`.
- The struct-key exemption, ruled and landed 2026-09-19: a struct whose fields hold an array or a collection at any depth is refused as a hashed key only where the compiler wrote the pair that matches it, so a struct declaring its own `equals` and `hash` keys a `Map` or a `Set`, and a `SortedMap` key is never refused for what its fields hold, since the `compareTo` the struct declares is what orders it.
  The refusal a compiler-written pair still earns gained a third fix, that pair with the changing field left out, worded once for the line that keys the container and once for a reader who only chose the type argument; two fixture pins and two unit-test pins moved onto it, three unit tests were added, and the happy-path fixture gained the two keys the exemption accepts.
- The narrative-phrasing sweep 09-todos.md carried, landed 2026-09-19: every comment, identifier and test description under `selfhost/` and `libs/` that described code as a person, a value that answers, asks, says, gives up, hands over or reads as something, now states the condition or names what the function returns, over 1,498 comment lines in 318 files, 240 test descriptions and the renames the checkpoint listed, `saysNothing`, `afterGivingUp` and `afterRetaining` among them.
  No message text changed and no exported member of `libs/std` was renamed; the three fixture pins whose test names the sweep touched moved with them, and rule 10 of the Diagnostics section in AGENTS.md is the standard the sweep applied.
- D4: cut the release whose seed makes the new std the one every consumer builds against.

## Standing rules for every phase

Any agent brief for a milestone states the quality bar explicitly: HIGH, parallel-ready, clean code, and the milestone's gate.
A compiler bug found mid-milestone is surfaced and fixed, never designed around.
Every grammar spelling the plan needs is signed off; a new one arising mid-milestone stops for sign-off first.
