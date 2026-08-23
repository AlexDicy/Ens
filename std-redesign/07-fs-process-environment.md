# @std.fs, @std.process, @std.environment

Rulings are settled; exact signatures are still to be written (fs is next in the level-3 pass).

## @std.fs

`Path` is a struct, and fs signatures take `Path`, not `string`.
`Path` carries I/O as well as text rules, in the pathlib style: `path.readText()`, `path.entries()`.
Pure methods stay pure: `join`, `parent`, `extension`, and `normalize` never touch the disk, so `normalize()` does not resolve symlinks, and a separate `realPath()` does and throws.
The existing pure form `absolute(path, workingDirectory)` stays alongside the no-argument convenience, so path rules remain testable with no file system.
`@std.path` retires into `@std.fs`, because `Path` returns `File` and `Metadata` and those take a `Path`.

No directory handles in the API.
`path.entries()` returns a lazy iterator that internally holds an open handle with a destructor.
One `metadata()` call returns a `Metadata` struct (kind, length, modified, isSymlink), with the predicates kept as sugar; today `exists`, `isFile`, and `isDirectory` are three separate native calls.

Still to decide when the signatures are written: symlink policy for the recursive walk, Windows UNC and drive-relative rules, atomic write, temporary files and directories as guard types, the `entries()` ordering guarantee, and TOCTOU guidance on `exists`.

## @std.process

One `run` with named arguments and defaults replaces today's eight overloads.
Named arguments plus defaults cover redirection too, since a `Writer?` parameter defaulting to null takes the capture target, so no builder type is needed.
The child inherits the parent's streams by default; capture is opt-in and goes to memory, not to caller-created temp files.
`ExitStatus` is a struct (code, signal, succeeded); spawn failure is a real throw, never the fake exit code 127 used today.
The Win32 quoting rules and the NUL-separated block builders stay in Ens, package-internal, with their unit tests.

Still to decide: stdin wiring, timeouts and kill, the PATH lookup rule and which environment it consults, what the destructor does to a still-running child, and whether the shell escape hatch keeps a deliberately awkward name.

## @std.environment

A dedicated `Environment` type owns the variables, taking its platform at construction rather than reading it, so the Windows case-insensitivity and merge rules stay unit-testable without the operating system.
Also here: `arguments()`, `platform()` returning an enum, `executablePath()`.
Still to decide: whether `arguments()` includes argv[0], and the return shape for an unknown executable path (today two different sentinels are used: an empty string and a dot).
