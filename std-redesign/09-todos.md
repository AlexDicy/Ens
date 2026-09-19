# Open questions and deferred items

Every item names when it is done: a milestone of 10-migration-plan.md, a phase, or the work that follows the redesign.

## Compiler consistency items

Two consistency items the C4 and pre-C5 work surfaced, all independent of the library migration, so they wait on nothing and block nothing.
A subclass method whose name a private base field uses is still refused, since `checkFieldMethodCollision` searches the flattened field list without the exemption private base fields gained on 2026-09-04; consistency would let it through, and it is a conservative refusal rather than an unsoundness.
A bare function reference stored into a local with no declared type, `let callback = twice;`, passes sema and then fails in codegen with "does not support a local of type '<error>' yet" (found 2026-09-08, no imports involved); an accepted program that cannot be compiled is a soundness matter, so it does not wait for the diagnostics review.

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

Code generation still names a type without the names of the file the message lands in: `unsupportedEntryShape` in `selfhost/codegen/src/driver.ens` reads the return type of an entry point through the context-free spelling, because codegen holds no link tables (2026-09-19).
It is the only user-facing message left on that path; the 320 other lines that read it in `selfhost/codegen/src` are EIR dumps and `Internal:` bug-catchers, which the context-free spelling is for.

`TypeNames` belongs to a file and not to a declaration, so it does not know a type parameter's scope: inside `class Holder<Kind>` a diagnostic about an imported `Kind` reads `Kind`, which is what that body calls the parameter (accepted 2026-09-19).
Threading the declaration's active type parameters into every spelling would fix it, and the context-free spelling this replaced was blind to the same thing.

A `FileDiagnostic` carries one related location, so an obligation failure shows the line that supplied the type arguments and the line inside the generic that holds the judgment, and nothing of the generics in between (2026-09-19).
A cascade two or more generics deep therefore shows its two ends only, which a chain of notes would fix once a diagnostic can carry a list of related locations.

The language's own runtime panics name neither the index nor the bound, while the standard library's collections name both (2026-09-19).
`array index out of bounds` in `selfhost/codegen/src/emit/addresses.ens`, `string byte index out of bounds` in `emit/text.ens`, and `slice range out of bounds` with `{member} range out of bounds` in `lower/builtins.ens` say only that a bound was passed, where `List` says `list index 2 is out of bounds for length 1`.
They are pinned by `tests/binding_intrinsics_abort` and `tests/stack_trace_panic`, and the standard library's review pass left them alone because they are the compiler's text rather than the library's.

`selfhost/driver/src/cst.ens:19` wraps a `FileSystemError` message in a prefix of its own, so `ens cst-dump gone.ens` prints `ens: could not read 'gone.ens': could not open 'gone.ens': nothing is there` (2026-09-19).
The path and the failure each appear twice, and `scripts/xmake_test.lua:1323` pins the substring `could not read` alone, so the wording is free to change.

`ens check libs/std` cannot check the standard library in any invocation, because the checker loads std as an ordinary package beside the implicit `@std` (2026-09-19).
With `--stdlib libs` that reports 32 problems, 31 of them naming a type against itself, `expected 'Path', got 'Path'` and once the same of `Platform`, since `Path` is loaded twice.
Without it there are 199, of which 126 are cross-package visibility refusals, since the `public` names of `@std.system` and `@std.collections.rawarray` are then read across a package boundary, and the other 73 follow from them (both counts measured 2026-09-19).
So a standard-library change has no sema gate faster than `ens test libs/std`, which takes about nine seconds.
The two loads of one file spell alike in that message because the per-file names read a type declared in the reporting file bare, which is truthful for one declaration and unreadable for two loads of it.

Emission is not reachability-based, so every function of every loaded module is lowered and linked whether or not a program can reach it (`lowerModule` in `selfhost/codegen/src/driver.ens` lowers "every function it defines", and no linker dead-strip flag is passed either).
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
