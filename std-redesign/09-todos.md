# Open questions and deferred items

Every item names when it is done: a milestone of 10-migration-plan.md, a phase, or the work that follows the redesign.

## Limits the process work accepts

A child is owned by one thread at a time until the threaded runtime gives it a lock or a documented single-owner rule.
The Windows output record's `pending`, `filled`, `consumed` and `ended` fields are plain stores, correct under that ownership and a race once two threads touch one child.

`selfhost/packages/src/tools.ens` stays on the old `@std.system` process family, and it is the one thing standing between D1 and deleting that family.
It relays both of a program's streams in the order they were written and bounds how long it waits for the program to say anything, and a single-threaded caller cannot do both over two separate streams.
A bounded wait that answers whether either stream is readable cannot say which one to read, and reading the other blocks until the child exits, which for a `git fetch` speaking on its error stream is a build that never comes back.
It moves when threads land and a reader of each stream carries its own bound.
It therefore keeps the old merged reader and that family's own `waitForOutput(milliseconds)`.
That call is a different one from the new `ChildProcess.waitForOutput(timeoutMillis)` despite the name, since the old one asks about one merged stream and the new one asks whether either of two separate streams can be read.

Two things follow from that family outliving D1, both of them limits on what D1 can reach.
`@std.text.lines` and its `LineBuffer` survive it, because the old `ChildProcess` is their only consumer and D1 cannot delete that, so the module goes when the threaded reader replaces it and not before.
And `@std.system` keeps exported names for as long as `selfhost/packages/src/tools.ens` reaches `SystemError`, `start` and `ChildProcess` across a package boundary, so the "all `public`, nothing `export`" that 07-fs-process-environment.md describes is not reachable at D1 either.
D1b confirmed both: those three are the only exported names left in the file, and `@std.text.lines` is still there for the `ChildProcess` behind them.

## Limits the C7b bridges accept

A Linux target reads metadata through the raw `statx` system call, which needs kernel 4.11 or newer.
There is no fallback: an older kernel answers `ENOSYS`, which surfaces as an ordinary `FileSystemError` of kind `Other`.
Should that floor ever matter, the fallback is `newfstatat` with one `struct stat` layout per architecture, which is what `statx` was chosen to avoid.
An architecture the compiler can target but whose `statx` number is unknown answers `ENOSYS` the same way, rather than reaching another call under a guessed number.

Win32 has no error code meaning `IsADirectory`, so a call that is refused a directory reports `PermissionDenied` on Windows and `IsADirectory` elsewhere.

`ens_fs_open` refuses a directory in every mode, which POSIX only requires of the modes that write.
POSIX.1 gives `open` the error `[EISDIR]` when "the named file is a directory and oflag includes O_WRONLY or O_RDWR", and says nothing of `O_RDONLY`, leaving `read` to answer `[EISDIR]` later when the implementation will not read a directory that way.
So the read mode asks `statx` with `AT_EMPTY_PATH` on Linux and `fstat` on macOS about the descriptor it already holds, and closes it before answering.
The check is on the descriptor rather than on the path, because a second lookup is both another system call and a window in which the path can become something else.
A system that will not say what the descriptor holds leaves the open to succeed, so the check never turns an ordinary file away.

`moveTo`'s promise to move in one step rests on `renameat2(RENAME_NOREPLACE)` on Linux and `renamex_np(RENAME_EXCL)` on macOS, both reached under the same rule as `statx`, so no libc has to carry a wrapper for either.
Where a file system will not carry the request out, and on an architecture whose `renameat2` number is unknown, the bridge falls back to reading the destination and renaming after it.
Two programs racing for the same name can then both be told they moved it, over a window of a few microseconds.
The exposure is none at all on ext4, xfs, btrfs, apfs and ntfs, and real on overlay and network file systems.

Nothing tells a caller or a test whether a Linux `ens_process_poll` is on the pidfd path or the 5 ms fallback a kernel without `pidfd_open` gets, so the fallback runs unmeasured on every supported machine.

The emitter tests that retarget a module prove a platform's half builds well-formed IR, and prove nothing about whether its numbers are true.
A negative control settled it.
Moving `Statx.modeOffset` from 28 to 29 in a disposable copy of the tree left every retarget test passing, while a wrong return type failed loudly.
So the verified triples are that many well-formed halves rather than that many checked layouts, and only `tests/fs_bridges` running on a host of that platform can say whether an offset or a system-call number is right.

## Limits the scratch retry accepts

The budget is sized against the 431 KB suite binary, the only size measured, so a scanner that holds a much larger program for longer than about two seconds still leaves the folder behind.
Malwarebytes is the only scanner the refusal was ever reproduced under, so nothing says whether another one holds a file longer or does not hold it at all.
A native reproduction outside the compiler never fired over 200 iterations, so the retry answers to the rate measured inside `ens test` rather than to a standalone reproduction of the race.
`Path.removeRecursively` keeps stopping at the first refusal (ruled 2026-09-17, the Rust shape rather than Go's remove-what-you-can), so a refusal that outlasts the budget costs the whole folder rather than the one file that was held.

## After Phase D

`TypeNames` belongs to a file and not to a declaration, so it does not know a type parameter's scope: inside `class Holder<Kind>` a diagnostic about an imported `Kind` reads `Kind`, which is what that body calls the parameter (accepted 2026-09-19).
Threading the declaration's active type parameters into every spelling would fix it, and the context-free spelling this replaced was blind to the same thing.

A `FileDiagnostic` carries one related location, so an obligation failure shows the line that supplied the type arguments and the line inside the generic that holds the judgment, and nothing of the generics in between (2026-09-19).
A cascade two or more generics deep therefore shows its two ends only, which a chain of notes would fix once a diagnostic can carry a list of related locations.

`ens_resolve_addr` answers with the registered symbol whose start is nearest at or below an address, with no upper bound, so a frame inside an internal runtime routine the symbol table has no entry for is attributed to the symbol that precedes it in the layout (2026-09-20).
Any such routine on the stack while `ens_capture_trace` runs therefore puts a line in the trace that names a function the program was never inside.
A "not a user frame" flag in the symbol table, registered for the internal routines and tested where the capture keeps an address, would fix it.
That is why a bounds, byte or range guard captures its trace itself and keeps the message it built live across the call, which costs one callee-saved register in every function holding such a guard: moving the capture into the routine that writes the text is free, and was measured putting a spurious innermost line in the traces of `tests/stack_trace_panic` and `tests/binding_intrinsics_range_abort`.

`ens check libs/std` cannot check the standard library in any invocation, because the checker loads std as an ordinary package beside the implicit `@std` (2026-09-19).
With `--stdlib libs` that reports 32 problems, 31 of them naming a type against itself, `expected 'Path', got 'Path'` and once the same of `Platform`, since `Path` is loaded twice.
Without it there are 199, of which 126 are cross-package visibility refusals, since the `public` names of `@std.system` and `@std.collections.rawarray` are then read across a package boundary, and the other 73 follow from them (both counts measured 2026-09-19).
So a standard-library change has no sema gate faster than `ens test libs/std`, which takes about nine seconds.
The two loads of one file spell alike in that message because the per-file names read a type declared in the reporting file bare, which is truthful for one declaration and unreadable for two loads of it.

Reachability-based emission gates function bodies and object files and not module counts: a module the program reaches no body in writes no object at all, and the descriptors it owns are defined in the core module's object instead, so a hello-world writes 8 of the 14 modules it loads (measured 2026-09-22 at -O2 on windows-x64, 39,022 bytes of objects against 40,993 before, and the same 166,400-byte executable).
Moving the module count means gating descriptors and therefore reachable types, which needs a ruling on how a call dispatched through a hierarchy roots its slot, and re-rooting the monomorphization closure, which `mono/requests.ens` cannot do today because it collects only generic calls.
Lowering on demand is the other stage left, and it buys the lowering and verifier time at the price of the coverage it removes: 132 functions across 59 of the 303 runnable fixtures are unreachable from their own entry point, and 21 of them are the declaration the fixture exists for.
Neither stage is ratified.

An object file holding no code makes `ld64.lld` write a `__unwind_info` entry with encoding 0, meaning no unwind information, at the address where the next object's code begins, which shadows that function's own entry (measured 2026-09-22 over arm64 macOS objects).
libunwind then finds no unwind information for any address in that function, the step out of it fails, and `_Unwind_Backtrace` ends the walk before the callback sees the frame, so `ens_capture_trace` keeps nothing and the trace prints empty.
Dropping the code-free objects from the link drops every such entry, which is what proved it.
Skipping the entry for a zero-sized input `__text` subsection, or ordering it before a real entry at the same address, is the upstream fix, and we consume a released lld through `runtime/lld/ens_lld.cpp`, so it is not actionable here: `reach.gate` never writing a code-free object is what keeps us clear of it.
The hazard predates reachability-based emission, which is the part a reader will need most: the pre-gating build already wrote two code-free objects, for the body-less `std.collections.collection` and `std.collections.iterator`, and their entries landed on `EncodingError.constructor` and `RawArray.read`, where no trace goes.
Gating added four more and moved every module's address, which put one of them on the function a trace is captured in and turned `tests/exc_main_unhandled` and `tests/stack_trace_propagation` red on macOS while both other platforms stayed green.

`Path.walk` offers no way to leave a folder out, so a caller that must not descend into one writes its own descent, and with it its own cycle guard.
`selfhost/packages/src/hashing.ens` is that caller: a tree's digest leaves `.git` out, and that has to be decided before the folder is entered rather than after everything under it has been handed over.
A lazy walk would let such a caller prune as it iterates, but the iterator protocol's `next()` carries no `throws`, so a walk that reads the file system while it is being consumed cannot report a folder it could not read.
Whoever revisits the walk surface therefore has two things to offer before that second cycle guard can go: a way to name the folders a walk does not enter, and an iterator that is allowed to fail.
The other thing a real consumer wanted is a relative path: `selfhost/build/src/sources.ens` answers paths relative to the folder it scanned, and with no `relativeTo` on `Path` it measures how much of an entry's path the folder wrote by joining a name to that folder and subtracting the name's length.

A call through a function value held in a field retains and releases the closure around every call, and a function-typed parameter is retained at entry and released at exit, so a comparator handed down a recursion pays two atomics per level (measured 2026-09-03 at -O2: 2ns per call through a parameter inside one function, 16ns through a field).
Escape analysis in code generation elides both (ratified 2026-09-04 as a post-redesign pass); until then `SortedMap` reads `this.order` at every step and recurses in its lookup rather than looping, since a loop retains and releases every node it moves onto.
When `@std.time` is designed, `Metadata.modifiedMillis` and `wait(long timeoutMillis)` take a proper duration or instant type; the names carry the unit until then.
`nearestDouble` allocates a digit buffer and a reading on every call, which the libc conversion it replaced did not, so a program parsing millions of doubles in a loop would notice.
The remedy when it matters is a fast path in front of the same rounding for the short inputs that need no buffer, and a buffer the conversion reuses.

Two questions about a name used as a value are open.
A static reached through a module-qualified type name, `renderer.Maker.build()`, reports `renderer.Maker` as a type rather than a value, since such a head became a refusal on 2026-09-21, so whether a module-qualified static head is supported at all is undecided.
An enum constant reached the same way, `renderer.Kind.Large`, reports that same refusal, so the question covers every type a module declares; until it is answered that refusal names no constant of its own, while the bare one does (2026-09-22).
A static-as-value message on a bare generic head can spell its fix only as `Holder.make(...)`, which fails when the call cannot infer the type arguments, while `Holder<T>.make(...)` would name a type parameter the use site does not have in scope.
The type-as-value refusal therefore names a generic type's static const, which needs no type argument, and never its static method (2026-09-22).

One shape still reports that a left side must be a variable, field, or array element where it is one.
Parentheses inside a target rather than around it, `(point).x = 2;` on a struct local, keep that sentence, while `(held).value = 1;` on a class and `(slots)[0] = 5;` on an array are accepted, so what the message can say waits on whether the struct shape should be refused at all.

`++`, `--` and a compound assignment on a nullable name the type and stop there: `x++` on an `int?` reports "The '++' operator works only on numbers, and this value has type 'int?'. Use it on an integer or a floating-point variable, for example 'count++'.", and `x += 1` reports "'+=' needs numbers on both sides, got 'int?' and 'int'.", neither of which mentions the null check that makes the line work (2026-09-21).
This is a nullability item rather than a safe-navigation one: a plain nullable local reads the same as `maybe?.value++`, whose text is pinned so a change has to move the pin.

"The compiler does not support assigning to this target yet." is written at two sites in `selfhost/codegen/src/lower/assignments.ens`, and rule 16 applies to one of them and maybe not the other, so neither is reworded until they are told apart.
The site in `lower` is now reachable from no program sema accepts, since sema admits only an identifier, a field, or an array element as a write target, which would make it a bug-catcher that takes the `Internal:` prefix; the site in `lowerThroughAddress` fires when an address cannot be computed for a field or an array element, which sema does accept, and no program reaching it has been found (2026-09-21).

The sema test suite's stand-in `@std.core` declares `Error.message` without `const`, while `libs/std/src/core.ens` declares it `export const string message` on an abstract class.
So a test program that assigns that field through a subclass constructor is clean against the stand-in and refused against the real library, which is how a fixture carried a second problem nobody saw until it was checked against `libs` (2026-09-21).

## The language server's replacement

The current C++ server is temporary; these are carried to its replacement rather than fixed in it.
It reports a spurious entry-point placement error on a single-file program, because it names a lone file's module after the file rather than treating the file as the program's main module, which is what `ens build <file>` does.
Its parser bounds no nesting, so deep shapes reach its stack; the replacement needs the bound the compiler's parser has.
It no longer checks what a lambda's body throws, because a lambda is held to the throws list of its target function type and the server does not track a lambda's target; the compiler owns the rule, so the cost is one missing diagnostic rather than a wrong one.
Three diagnostics anchor to the wrong node: the interface-widening error to the whole class declaration (`ThrowsAnalyzer.cpp:486`), "'try' is not needed here" to the call instead of the keyword (`ThrowsAnalyzer.cpp:298`), and "cannot be 'final'" to the whole method declaration; underneath, `lsp/server/DiagnosticBridge.cpp` computes a range as `startCh + length` on one line, so a multi-line node's range runs past its line.
Its type model carries no thrown-type list on a function type, so a type argument that appears only in a `throws` list can never be inferred there.
It does not treat a value of `Bag<int>`, where `Bag<T> extends Iterable<T>`, as an `Iterable<int>`, which `tests/interface_extends.ens` shows as spurious assignment and `override` errors.
Its parser rejects a local declaration whose type is a parenthesized function type with a `throws` list, `(() -> int throws Failure) safe = ...`, and misreads every statement after it in the block.
A generic static call result does not feed its type-argument inference: after `let words = List<string>.of([...])`, `countOf(words)` reports "Cannot infer type argument 'T'" while the compiler infers it.
It lacks the compiler's dedicated function-value and array-element interpolation messages and falls through to its generic "only string, integer, ..." line for those shapes.
It reports "Imported name 'Comparable' conflicts with an existing declaration" for `import Comparable from @std.core;`, because an implicitly imported name is bound before explicit imports and the two are treated as rival declarations rather than the same one; the compiler accepts the redundant import.
It checks no struct conformance, so a struct that implements an interface without providing a requirement, or provides one whose signature does not match, is reported by the compiler alone.
It also lacks the conformance hint the compiler appends when a struct or a primitive flows into an interface-typed slot, so its message stops at "Cannot assign value of type 'Note' to variable of type 'Speaker'".
It resolves a constructor's `this.field` shorthand with a plain `findFieldIndex` at `lsp/frontend/semantic/Analyzer.cpp:3616`, and `analyzeImplicitConstructorAssignments` at line 3765 does the same, so it accepts two shorthands the compiler refuses as of 3ba7b80 and a53a21a: one binding a private base field, and one binding a field that is public in another package.
It runs no `checkMemberAccess` on that path, though it has one at line 5378 for ordinary member expressions, so the fix there is the same shape as the compiler's (2026-09-21).
Its remaining `findFieldIndex` call sites carry no private-base-field exemption, so hover, go-to-definition and rename (`lsp/server/LanguageServer.cpp`) can resolve a name to a private base field that a subclass member shadows, though the sites that report a diagnostic now carry the exemption (2026-09-21).
It checks no duplicate struct field, so a struct that declares one name twice is reported by the compiler alone, which says "Field 'y' is already declared in 'Pixel'"; its struct field loop at `lsp/frontend/semantic/Analyzer.cpp:1495-1515` pushes every field without the `findFieldIndex` check the class loop makes at line 2023 (2026-09-21).
Its own copies of three of the messages rewritten on 2026-09-21 keep the weaker wording, the `this.field` shorthand refusal at `lsp/frontend/semantic/Analyzer.cpp:2537` and the cross-package member and constructor refusals at lines 5391 and 5634.
It accepts a module-qualified type name as a value at `lsp/frontend/semantic/Analyzer.cpp:7181-7187`, recording the type as the expression's own, which the compiler refuses as of 2026-09-21.

## Reminders

Constructors cannot be `throws` (selfhost/sema/src/phases/members.ens:1106, deliberate), which is why anything whose creation does I/O uses a static factory: `TemporaryDirectory.create(prefix)`, `TemporaryFile.create(prefix)`, `Path.open()`.
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
