# @std.fs, @std.process, @std.environment

## @std.fs layout

```
libs/std/src/fs.ens          Path
libs/std/src/fs/error.ens    FileSystemError, ErrorKind
libs/std/src/fs/file.ens     File
libs/std/src/fs/metadata.ens Metadata, EntryKind
libs/std/src/fs/entry.ens    Entry
libs/std/src/fs/temp.ens     TemporaryDirectory, TemporaryFile
```

```ens
// @std.fs.error
export class FileSystemError extends Error {
    export const ErrorKind kind;
    export const Path path;
    export constructor(this.message, this.kind, this.path, Error? cause = null);
}

export enum ErrorKind {
    NotFound,
    PermissionDenied,
    AlreadyExists,
    NotADirectory,
    IsADirectory,
    DirectoryNotEmpty,
    NoSpace,
    Other,
}
```

## Path

```ens
// @std.fs
// A location in the file system, and the operations that read or change what is there. The text
// rules never touch the disk, so their answers are the same whatever exists; the operations below
// them do, and report failure as a FileSystemError.
//
// '/' separates the parts of a path, on every platform. Text arriving from the operating system or
// a command line is converted once, at the boundary, with `fromNative`.
export struct Path implements Comparable<Path> {
    export constructor(string text);

    // ---- rules about the text ----

    export isEmpty() -> bool;

    // This path and `part` with one separator between them. Joining an absolute part answers the
    // part alone, because an absolute path names its place on its own.
    export join(string part) -> Path;
    export join(Path part) -> Path;

    // The folder one level up: null at a root, and for a name with no folder written before it.
    export parent() -> Path?;

    // The last part of the path, "" at a root; the extension after the last '.' of that name,
    // without the dot and "" when there is none; and the name without that extension.
    export fileName() -> string;
    export extension() -> string;
    export stem() -> string;

    export isAbsolute() -> bool;

    // The path with '.' parts, repeated separators, and every '..' that has a part to remove
    // dropped. Symbolic links are text like any other here, so the answer can name a different
    // place than the original; resolving what is really on disk is `realPath`.
    export normalize() -> Path;

    // This path read against `workingDirectory` and normalized. The overload with no argument
    // reads it against the directory the program was started in.
    export absolute(Path workingDirectory) -> Path;
    export absolute() -> Path;

    // Byte order of the text. Two paths are equal only when their text is equal: "a/b" and
    // "a/./b" name the same place but are different paths until normalized.
    export override compareTo(Path other) -> int;

    // The text itself.
    export override toString() -> string;

    // Text written the way `platform` writes a path, as a Path in the form above: on Windows '\'
    // becomes '/', everywhere else the text is kept exactly, because '\' is an ordinary character
    // in a name there.
    export static fromNative(string text, Platform platform) -> Path;

    // ---- what is there ----

    // What is at this path, or null when nothing is. A symbolic link answers for what it points
    // to. Asking is one call, and the predicates below are shorthands over it.
    export metadata() -> Metadata? throws FileSystemError;

    // Whether something is there. Answering true proves nothing about a moment later, so opening
    // and catching NotFound beats asking first and opening second.
    export exists() -> bool;
    export isFile() -> bool;
    export isDirectory() -> bool;

    // The path with every symbolic link resolved, as the operating system reports it.
    export realPath() -> Path throws FileSystemError;

    // ---- reading and writing files ----

    export readBytes() -> byte[] throws FileSystemError;
    export readText() -> string throws FileSystemError;
    export writeBytes(byte[] content) throws FileSystemError;
    export writeText(string content) throws FileSystemError;

    // Writes to a sibling temporary file and moves it over this path in one step, so a reader sees
    // the old content or the new and never a half-written file.
    export writeBytesAtomic(byte[] content) throws FileSystemError;
    export writeTextAtomic(string content) throws FileSystemError;

    // Open for reading, open truncated for writing, open for writing at the end.
    export open() -> File throws FileSystemError;
    export create() -> File throws FileSystemError;
    export append() -> File throws FileSystemError;

    // ---- directories ----

    // What is directly inside this directory, one entry at a time, in ascending byte order of
    // name, so a walk visits the same names in the same order however the file system enumerates
    // them. '.' and '..' are not entries.
    export entries() -> Iterator<Entry> throws FileSystemError;

    // Every entry under this directory, depth first, each directory's entries in the same order as
    // `entries`. A symbolic link is reported and not followed, so a cycle cannot trap the walk.
    export walk() -> Iterator<Entry> throws FileSystemError;

    // Creates this directory and every missing parent. A path that is already a directory is left
    // alone.
    export createDirectories() throws FileSystemError;

    // The same, and answers whether this call is the one that created the directory itself: two
    // programs racing for the same name get different answers, and the one answered true may
    // treat the directory as its own.
    export createDirectoryExclusive() -> bool throws FileSystemError;

    // ---- moving and removing ----

    // Moves what is at this path to `destination` in one step, and answers whether this call is
    // the one that moved it: false when something was already there. Both paths have to be on the
    // same volume, because a move across volumes would have to copy.
    export moveTo(Path destination) -> bool throws FileSystemError;

    // Copies the file at this path to `destination`, replacing what was there.
    export copyTo(Path destination) throws FileSystemError;

    // Removing a file refuses a directory, and removing a directory requires it empty; what is
    // inside is the caller's to decide. `removeRecursively` decides it: everything under the path,
    // then the path, and a symbolic link is removed itself, never followed.
    export removeFile() throws FileSystemError;
    export removeDirectory() throws FileSystemError;
    export removeRecursively() throws FileSystemError;
}
```

## File, Metadata, Entry, temp guards

```ens
// @std.fs.file
// An open file. Reading and writing speak the stream contracts, so a file goes anywhere a Reader
// or Writer goes.
export final class File implements Reader, Writer {
    export override read(byte[] buffer) -> long throws IoError;
    export override write(byte[] data) throws IoError;
    export override flush() throws IoError;

    // Hands the file back to the operating system, reporting a write that could not be finished.
    // Asking twice is allowed and the second answer is silence.
    export close() throws IoError;

    // Closes, keeping any failure to itself. A file that was written and matters is closed with
    // `close`, so the failure has somewhere to go.
    destructor();
}
```

```ens
// @std.fs.metadata
// What is at a path.
export struct Metadata {
    export const EntryKind kind;

    // The length in bytes; 0 for a directory.
    export const long length;

    // When the content last changed, in milliseconds since 1970-01-01 UTC.
    export const long modifiedMillis;
}

export enum EntryKind {
    File,
    Directory,
    Symlink,
    Other,
}
```

```ens
// @std.fs.entry
// One thing inside a directory: where it is, its own name, and what it is, known without another
// look at the disk. A symbolic link reports Symlink, never what it points to.
export struct Entry {
    export const Path path;
    export const string name;
    export const EntryKind kind;
}
```

```ens
// @std.fs.temp
// A directory or file that exists for as long as the value does. Made under the system's temporary
// location with a name no other call answers, and removed when the value is dropped.
export final class TemporaryDirectory {
    export static create() -> TemporaryDirectory throws FileSystemError;
    export path() -> Path;

    // Dismisses the cleanup and answers the path, which the caller now owns.
    export keep() -> Path;

    // Removes the directory and its contents, keeping any failure to itself.
    destructor();
}

export final class TemporaryFile {
    export static create() -> TemporaryFile throws FileSystemError;
    export path() -> Path;
    export keep() -> Path;

    // Removes the file, keeping any failure to itself.
    destructor();
}
```

## fs decisions

`metadata()` follows symbolic links; `Entry.kind` does not, which is what makes `walk` cycle-proof; a followed answer can never say Symlink, so `Metadata` has no isSymlink and an lstat-shaped call is deferred.
`modifiedMillis` is the no-time-type interim; see the reminder in the TODOs.
`removeRecursively` is included because `TemporaryDirectory`'s destructor needs the operation anyway; the name is long on purpose.
UNC: `\server\share` arrives via `fromNative` as `//server/share`, is absolute, and `normalize` keeps a leading `//`. Drive-relative `C:foo` is unsupported and documented as relative text; `C:/foo` stays absolute.
`entries()` promises ascending byte order of name, each directory read fully and sorted before yielding; determinism is worth the freeze.
One `File` type for both directions; writing a read-opened file throws `IoError`.
`metadata()` returns null for the one expected absence and throws for everything else, which keeps `exists()` a non-throwing shorthand.
TOCTOU guidance lives on `exists()` itself.
`currentDirectory()` lives in `@std.environment`; `absolute()` with no argument reads it from there, and the circular import this needs is legal.
Statics like `TemporaryDirectory.create()` exist because constructors cannot throw (selfhost/sema/src/phases/members.ens:501); `Path.open()` returning `File` is the same rule.

## @std.process

```
libs/std/src/process.ens          run, runShell, spawn, CommandOutput, ExitStatus,
                                  ChildProcess, ProcessError, ErrorKind
libs/std/src/process/native.ens   argument blocks, Win32 quoting (package-internal)
```

```ens
// @std.process
// Starting other programs. No shell takes part unless asked for by name, so an argument reaches
// the program exactly as written, spaces, quotes and all.

export class ProcessError extends Error {
    export const ErrorKind kind;
    export const string program;
    export constructor(this.message, this.kind, this.program, Error? cause = null);
}

export enum ErrorKind {
    NotFound,
    PermissionDenied,
    Other,
}

// How a program ended: its exit code when it exited, or the signal that ended it. Exactly one of
// the two is meaningful, and `succeeded` is the question most callers ask. On Windows the signal
// is always 0 and an abnormal end surfaces through the code.
export struct ExitStatus {
    export const int code;
    export const int signal;

    export succeeded() -> bool;
}

// What a finished program left behind. The two texts are empty unless the run captured them.
export struct CommandOutput {
    export const ExitStatus status;
    export const string stdout;
    export const string stderr;
}

// Runs `program` and waits for it to finish. The child reads and writes this process's own
// streams, so its output appears as it is written; with `captureOutput` the two streams are
// collected and answered instead. `environment` gives the child exactly those variables, and null
// hands this process's own on unchanged. Failing to start is an error; a program that started and
// failed is a `status` to inspect.
export run(Path program, string[] arguments = [],
           Path? workingDirectory = null,
           Environment? environment = null,
           bool captureOutput = false) -> CommandOutput throws ProcessError;

// Runs `commandLine` through the operating system's command interpreter, with everything that
// implies: the shell splits, expands, and interprets the line. An argument built from input
// belongs in `run`, which interprets nothing.
export runShell(string commandLine,
                Path? workingDirectory = null,
                Environment? environment = null,
                bool captureOutput = false) -> CommandOutput throws ProcessError;

// Starts `program` and hands it back while it runs.
export spawn(Path program, string[] arguments = [],
             Path? workingDirectory = null,
             Environment? environment = null) -> ChildProcess throws ProcessError;

// A program this one started and is still running.
export final class ChildProcess {
    // What the child writes to each of its two streams. Until threads land, draining one stream
    // to the end while the child fills the other can leave both sides waiting; reading the stream
    // the child actually writes to, or capturing through `run`, avoids that.
    export stdout() -> BufferedReader;
    export stderr() -> BufferedReader;

    // The child's standard input. Closing it is how the child learns the input has ended.
    export input() -> Writer;
    export closeInput() throws IoError;

    // The exit status, waiting for the child to finish first. Output not yet read is read and
    // thrown away, because asking for the status is asking for the child to be finished with.
    // Asking again answers the same status.
    export wait() -> ExitStatus throws ProcessError;

    // The same, giving up after `timeoutMillis`: null means the child is still running.
    export wait(long timeoutMillis) -> ExitStatus? throws ProcessError;

    // Ends the child now, without waiting. The status then reports how it ended.
    export kill() throws ProcessError;

    // Lets the child go: it keeps running with nothing reading it. A child that should not
    // outlive this program is killed or waited for instead.
    destructor();
}
```

## process decisions

Child streams are separate (`stdout()`/`stderr()`), with the single-threaded deadlock hazard documented as interim until threads land; no merged mode exists.
`run(captureOutput: true)` redirects at the operating-system level internally, so capture cannot deadlock and stdout and stderr stay separate.
The destructor detaches and never kills or waits; ending a child is explicit.
PATH rule: a program name with no separator is searched in the PATH of the environment the child will receive, falling back to the parent's when none was given.
`environment:` replaces rather than merges: patching is `Environment.current()` plus `set`, a clean slate is `Environment.empty(platform)` plus `set`, and what is passed is exactly what the child sees.
Python and Go use the same replace semantics for a child's environment; Rust's override-style Command needed four methods to express the same two intents.
`runShell` is the shell escape hatch, named so it is greppable.
`wait(long timeoutMillis)` is the no-time-type interim, same as `modifiedMillis`.

## @std.environment

```ens
// @std.environment
// What surrounds the running program: its variables, its arguments, and where it is.

export enum Platform {
    Windows,
    Linux,
    MacOS,
}

// The system the program was built for. The members are exactly the targets the compiler can
// build for, so this can never answer anything else, and a new target grows the enum.
export platform() -> Platform;

// The arguments the program was started with, not counting the program's own name.
export arguments() -> string[];

// The path of the running program itself, or null when the operating system would not report it.
export executablePath() -> Path?;

// The directory the program was started in, or "." when the operating system would not report
// it. It is read, never written: a program that wants to work somewhere else joins this onto the
// paths it uses.
export currentDirectory() -> Path;

// A set of variables, matched by name the way `platform` matches them: on Windows two names that
// differ only in ASCII case are the same variable. The matching is a rule about the values, so an
// Environment built for any platform behaves the same on every platform.
export final class Environment {
    // The variables this process inherited, as a snapshot: changing it changes nothing outside,
    // and a child is given the variables it should see when it is started.
    export static current() -> Environment;

    // No variables at all, matched by `platform`'s rule.
    export static empty(Platform platform) -> Environment;

    export get(string name) -> string?;
    export set(string name, string value);
    export remove(string name) -> bool;
    export contains(string name) -> bool;
    export length() -> long;
    export isEmpty() -> bool;

    // The names, in ascending byte order, so a walk is deterministic.
    export names() -> string[];

    export copy() -> Environment;
}
```

## environment decisions

`Platform` has no Unknown member: the value is a build-time fact from a closed target list, and a new target grows the enum, which forces every exhaustive switch to answer for it instead of masquerading as Linux.
`Environment` is not Iterable in v1; `names()` plus `get` covers enumeration deterministically.
`arguments()` excludes the program's own name; the program's identity is `executablePath()`.
The old public merge machinery stops being API; block building moves to `@std.process.native` and name matching lives inside `Environment.set`.

## @std.system (internal)

One file, every `external` declaration in the library, all `public`, nothing `export`.
Contents: the file bridges (open, read, write, close, metadata, listing, create, remove, rename, realpath), the process bridges (spawn with OS-level redirection, pipe read, wait with and without timeout, kill, release), the environment bridges (variables snapshot, argv, executable path, cwd), and `errorKindFromErrno(int)` so the errno mapping exists in exactly one place.
