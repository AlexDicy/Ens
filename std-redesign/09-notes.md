# Limits and design notes

What the code accepts on purpose and why, kept beside the design it belongs to.
Open work is a GitHub issue, never an entry here.

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

## Limits the scratch retry accepts

The budget is sized against the 431 KB suite binary, the only size measured, so a scanner that holds a much larger program for longer than about two seconds still leaves the folder behind.
Malwarebytes is the only scanner the refusal was ever reproduced under, so nothing says whether another one holds a file longer or does not hold it at all.
A native reproduction outside the compiler never fired over 200 iterations, so the retry answers to the rate measured inside `ens test` rather than to a standalone reproduction of the race.
`Path.removeRecursively` keeps stopping at the first refusal (ruled 2026-09-17, the Rust shape rather than Go's remove-what-you-can), so a refusal that outlasts the budget costs the whole folder rather than the one file that was held.

## The ld64 unwind-info hazard

An object file holding no code makes `ld64.lld` write a `__unwind_info` entry with encoding 0, meaning no unwind information, at the address where the next object's code begins, which shadows that function's own entry (measured 2026-09-22 over arm64 macOS objects).
libunwind then finds no unwind information for any address in that function, the step out of it fails, and `_Unwind_Backtrace` ends the walk before the callback sees the frame, so `ens_capture_trace` keeps nothing and the trace prints empty.
Dropping the code-free objects from the link drops every such entry, which is what proved it.
Skipping the entry for a zero-sized input `__text` subsection, or ordering it before a real entry at the same address, is the upstream fix, and we consume a released lld through `runtime/lld/ens_lld.cpp`, so it is not actionable here: `reach.gate` never writing a code-free object is what keeps us clear of it.
The hazard predates reachability-based emission, which is the part a reader will need most: the pre-gating build already wrote two code-free objects, for the body-less `std.collections.collection` and `std.collections.iterator`, and their entries landed on `EncodingError.constructor` and `RawArray.read`, where no trace goes.
Gating added four more and moved every module's address, which put one of them on the function a trace is captured in and turned `tests/exc_main_unhandled` and `tests/stack_trace_propagation` red on macOS while both other platforms stayed green.

## Rulings on type parameters and statics

A static-as-value message on a bare generic head can spell its fix only as `Holder.make(...)`, which fails when the call cannot infer the type arguments, while `Holder<T>.make(...)` would name a type parameter the use site does not have in scope.
The type-as-value refusal therefore names a generic type's static const, which needs no type argument, and never its static method (2026-09-22).

A type parameter in scope wins its name in every position, so inside `class Bin<Kind>` the name `Kind` is the parameter whether it is written as a type, as `Kind.Large`, as `Made(3)`, or as a static head (ruled 2026-09-22).
The file's own class, struct, interface or enum of that name, a type or a module it imports, a function it declares, and a function an implicitly imported module declares are all unreachable under that name inside the declaration; only a local declared further in takes the name back.
A class header declares the parameter inside the file's own declarations, an inner declaration wins for the whole of its scope, and resolving by position instead would let one name mean two things with nothing in the source marking where the meaning switches.
It leaves one rule with no exceptions, which the divergent cases it replaced did not, and a type parameter can be neither constructed nor called and has no constants or statics of its own, so nothing is lost by refusing every use of it as a value.

## The linker bridge is never unloaded

Unloading `ens-lld` with `FreeLibrary` after a single-threaded link crashes at process exit inside `rpmalloc_initialize` from an unloaded page; `ens` never unloads the bridge, so nothing reaches it (2026-09-23).

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
A duration or instant type for `Metadata.modifiedMillis` and `wait(long timeoutMillis)`, once `@std.time` is designed; the names carry the unit until then.
