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

Emission was not reachability-based until 2026-09-21, so every function of every loaded module was lowered and linked whether or not a program could reach it, and the paragraphs below are the record of what that cost.
Two costs measured, which compound: routing `print` through `@std.system` took a hello-world from 2 modules and 151,552 bytes to 12 modules and 238,592 bytes (2026-09-05), and C6 then added all of `std.text.string`, 32 KB of a hello-world's 160 KB of objects, since `lower/index.ens` pushes every bodied binding member into its module's function list.
Together they cost 21% of suite wall time and 34% of `codegencheck`'s, measured against a 177-second baseline; the bootstrap moved only 5%, so the Ens scanning members are not the cost and a fixed per-program charge is.
C9 removes the `@std.system` half by shrinking that module, but the binding half is permanent, since bindings load implicitly into every program.
Ratified 2026-09-05: carry both until after Phase D, then make emission reachability-based, rooted at the entry point plus what data can reach (vtable and interface-table slots, the `hash`/`equals`/`toString` descriptor slots, and every `export` in a library build).

C7c added a third charge of the same kind, and it is recorded here so it is visible to whoever lifts it rather than argued from memory.
The twelve `ens_fs_*` bridges plus `ens_current_directory` are declared in `@std.system` rather than in the `@std.fs` files that call them, so every program synthesizes all thirteen though a hello-world reaches none of them.
Measured on 2026-09-06 at -O2, with the declarations first in the `@std.fs` files and then in `@std.system`: a hello-world went from 167,771 to 177,901 bytes of objects and from 276,480 to 282,624 bytes of executable on windows-x64, and from 207,376 to 217,088 bytes of objects on linux-x64.
The module count did not move, staying at 15 either way, because `@std.system` is already in every program through the `@std.io.print` prelude.
The single place was chosen over the per-program cost with that measurement in hand (ratified 2026-09-06), so the `@std.system` half of the charge grows before C9 shrinks it, and what removes it in the end is reachability-based emission rather than the module's size.
Moving the consumers then paid the charge back and more, because `@std.path` became a shim over `Path` and so could no longer be reached from `@std.system`, which took `std.path` out of the prelude chain: a hello-world ended at 14 modules and 166,543 bytes of objects on windows-x64 and 202,400 on linux-x64, below the 15 modules and 167,771 bytes it started C7c at.
C8 then lost all of that and more, unmeasured at the time: it pointed `@std.system` at `@std.process.native`, which imports `Environment`, `Platform` and `Path`, so every program loaded `@std.environment` and the whole of `@std.fs` behind it, and a hello-world reached 26 modules and 420,360 bytes of objects on windows-x64 and 506,792 on linux-x64.
Splitting the shapes that are a rule about values alone into `@std.process.blocks`, which imports only `List` and `StringBuilder`, put it back to 14 modules and 180,987 bytes on windows-x64 and 215,248 on linux-x64 (measured 2026-09-16 at -O2).
What stands above the C7c figure is `@std.system`'s own object, 71,027 bytes of the 180,987, which is where C8's ten new bridges and their wrappers sit and what the single-place ruling accepts.
The suite was timed across that split as well, two runs at each commit with a checkout before every run so all four met the same caches: 335s and 323s after it, 480s and 429s before it (2026-09-17).
So the split took between 94 and 157 seconds off a suite that had been running 429 to 480, a fifth to a third of it, almost all of it in `codegencheck` at -O2; the repeats differ by 12s after and 51s before, so the direction and the rough size hold while the precise figure does not.
That is no recomputation of the 21% above, which was measured against a 177-second baseline on an older tree, and it is not a second saving beside the object numbers either, since an executable carries what the linker keeps of those objects and nothing passes a dead-strip flag.
What it says is that until 673a29d the charge reachability-based emission is meant to lift had grown to about a quarter of this suite.

That charge is lifted for function bodies and object files, landed 2026-09-21 in fc7ded5 and 39ae7ce, and untouched for module counts.
`selfhost/codegen/src/reach/` holds the walk, the account `--explain-reachability` prints, the closure check that refuses to emit while any reference points at a dropped body, and the two rules that keep an object for `@std.core` and for a module that owns a descriptor.
The filter sits between lowering and the optimization pipeline, so every function of every program is still lowered and still verified, and `codegencheck` keeps its coverage of the lowering paths with an empty skiplist and no fixture edit.
Measured at -O2 on windows-x64: a hello-world went from 183,054 to 40,977 bytes of objects and from 272,384 to 166,400 bytes of executable, the compiler's own binary from 8,500,736 to 6,711,808 bytes, `codegencheck` from 133 to 78 seconds, and the Linux suite from 4m32s to 2m56s.
The root set is three things and it is smaller than the 2026-09-05 ratification described: the entry point, every slot of every emitted descriptor with interface-table slots counted apart from vtable slots, and every `export` when there is no entry point.
Destructors, lambdas, constructors, lazy initializers, generic members and tests are deliberately not roots, because EIR names every symbol a function references and the walk reaches each one through its referencing site.
Rooting destructors from their class, which the ratification implies, would have discarded 74 of the 132 droppable fixture functions measured before the work began.
The ratification's remark about a linker dead-strip flag is a dead end: LLVM emits one `.text` per object and `_ens_symtab` takes the address of every recorded function from a live global constructor, so relinking a hello-world with and without `/OPT:REF` gives a byte-identical result.

The module count stays where it was, because descriptors are still emitted unconditionally, so every module that declares a type keeps an object and a hello-world stays at 14 modules while writing 13 of them.
Moving it means gating descriptors and therefore reachable types, which needs a ruling on how a call dispatched through a hierarchy roots its slot, and re-rooting the monomorphization closure, which `mono/requests.ens` cannot do today because it collects only generic calls.
Lowering on demand is the other stage left, and it buys the lowering and verifier time at the price of that coverage: 132 functions across 59 of the 303 runnable fixtures are unreachable from their own entry point, and 21 of them are the declaration the fixture exists for.
Neither stage is ratified.

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

Four user-facing messages are weaker than the rules in AGENTS.md ask for, all found while the name-as-value work landed on 2026-09-21.
`No field '<name>' on type '<type>'.` names the problem and offers no fix, and it now has a pin in `tests/name_as_value_errors.ens`, so rewording it moves a pin rather than going unmeasured.
The cross-package member refusal ends "Mark it 'export' to use it from another package." without naming the module to mark it in, while the sibling message for a top-level function does say "in module '<path>'".
So a writer who does not own that package cannot tell from the text where the fix belongs.
A module-qualified function name used as a value reports "Module 'renderer' has no type named 'configure'" even when `configure` exists and is public, which is false, and it answers a question about a function with a sentence about types.
Fixing it means separating three causes in `analyzeNamespaceMemberType`: a name that exists and is reachable, one that exists and is private, and one that does not exist, with `types.notVisibleMessage` carrying the private case.
The instance-method-as-value message states the fact before the fix, "'bump' is a method of 'Counter', and a method's name is not a value. Call it as 'bump(...)'.", while the pre-existing static sibling does not, "Static method 'make' of 'Counter' must be called; write 'Counter.make(...)'.", so the family converges when either one next moves.

A parenthesized left side of `=` skips the const-field rule, because `analyzeAssignment` keys `checkConstFieldWrite` on the target node being a member expression (verified identical before and after 2fb2f32, so it predates that work).
`(this.value) = 99;` inside the declaring constructor and `(held.value) = 99;` from outside it both report "The left side of '=' must be a variable, field, or array element." and nothing about const.
That message is also false on its own terms there, since the left side is a field in parentheses, so the rule and the wording are one item.

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
