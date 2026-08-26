# @std.testing

`test "..." { }` is a language declaration, so the module holds the failure type and assertions only.

```ens
// @std.testing
// What a test throws to fail, and the assertions that throw it.

export class TestFailure extends Error {
    export constructor(this.message, Error? cause = null);
}

// Equality by ==. A failure prints both values, and for collections and multi-line text it points
// at the first difference rather than printing two long lines.
export assertEqual<T>(T actual, T expected) throws TestFailure;
export assertNotEqual<T>(T actual, T expected) throws TestFailure;

export assertTrue(bool condition, string message = "expected condition to be true") throws TestFailure;
export assertFalse(bool condition, string message = "expected condition to be false") throws TestFailure;

export noreturn fail(string message) throws TestFailure;

// Equality within `tolerance`, for values that arrive through arithmetic rather than exactly.
export assertNear(double actual, double expected, double tolerance) throws TestFailure;

// Runs `body` and answers the E it threw; a body that throws nothing, or something that is not an
// E, fails the test. The answer is the error itself, so the test goes on to check its kind or its
// message.
export assertThrows<E: Error>(() -> void body) -> E throws TestFailure;
```

## Decisions

`assertThrows` is generic over the error type and returns the caught error, because the expected condition cannot be a parameter: every module's `ErrorKind` is a different enum type.
The caller checks `kind` or `message` with the assertions that already exist.
This uses class-typed bounds (`E: Error`), decided in the language prerequisites.
Structured diffs live in `assertEqual`'s implementation, not its signature.
`assertEqual` keeps exact == on floats, which is sometimes right; `assertNear` is the tool for the rest.
No test may depend on any unspecified order: map iteration, set iteration, or sort tie order.
Each std module keeps the happy-path file and errors file split.
