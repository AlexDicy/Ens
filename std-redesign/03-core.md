# @std.core

Implicitly imported into every module.
Holds the error base, the stack frame, and the two generic contracts.

```ens
// The names every module can use without writing an import: the base class of every error, the
// frames a captured trace resolves to, and the two contracts a type implements to be ordered or
// copied.

// What a failure was. A subclass names the domain that failed, such as the file system, a process,
// or parsing, and carries a `kind` naming the condition within that domain. Nothing throws an
// `Error` itself, because the type is what a catch clause selects on.
export abstract class Error {
    // What failed, naming the value it failed on.
    export const string message;

    // The error this one was raised in response to, or null when it wraps nothing. Reading a chain
    // from the outside in goes from what failed to why.
    export const Error? cause;

    // Return addresses captured where this error was thrown, symbolicated only when read.
    private long[]? frames;

    protected constructor(this.message, this.cause = null);

    // The message, then each cause beneath it, one per line, innermost last.
    export override toString() -> string;

    // The frames as text, innermost first, and empty for an error that was built but never thrown.
    export stackTrace() -> string;

    // The same frames as values. The compiler lowers both of these to the trace runtime, so the
    // bodies here are never emitted.
    export stackFrames() -> StackFrame[];
}

// One frame of a captured trace: what was running, and where in the source.
export struct StackFrame {
    export const string function;
    export const string file;
    export const int line;
}

// A total order over T. `compareTo` answers negative when the receiver sorts before `other`, zero
// when neither sorts first, and positive when it sorts after; only the sign is meaningful. A
// container that has to order values without being told how asks for this, while a one-off order is
// a comparison passed to the call.
export interface Comparable<T> {
    compareTo(T other) -> int;
}

// A duplicate whose top level shares nothing with the original. What it holds is still shared, so
// copying a list gives a second list holding the same elements.
export interface Copyable<T> {
    copy() -> T;
}
```

## Decisions embodied here

`Error` is abstract; a concrete base makes `throw new Error("...")` the lazy default, which turns every catch into a catch-all.
Migration cost accepted: 23 existing `new Error(...)` sites break and most should throw `TestFailure`.
The constructor is `protected`, which is verified to reach cross-package subclasses.
`StackFrame` is a struct with no constructor; exported `const` fields make it buildable from a context-typed literal.
The field stays named `function`, since the closure spelling will not reserve that word.
`Hashable` does not exist and `@std.hash` retires: every class and struct already has `hash` through runtime dispatch, so the compiler rejects unusable key types (arrays, external handles) at the instantiation site instead.
`ArgumentError` does not exist: contract violations panic, and recoverable bad input is absence (`T?`).
No `Displayable` interface: `toString` already dispatches from the runtime type for every value.
