# @std.testing

`test "..." { }` is a language declaration, so the module holds the failure type and assertions only.

```ens
// @std.testing
// What a test throws to fail, and the assertions that throw it.

export class TestFailure extends Error {
    export constructor(string message, Error? cause = null);
}

// Equality by ==, which compares contents for a string, an array, and a collection. A failure
// prints both values, and points at the first difference rather than printing two long lines.
export assertEqual<T>(T actual, T expected) throws TestFailure;
export assertNotEqual<T>(T actual, T expected) throws TestFailure;

export assertTrue(bool condition, string message = "expected condition to be true") throws TestFailure;
export assertFalse(bool condition, string message = "expected condition to be false") throws TestFailure;

export noreturn fail(string message) throws TestFailure;

// Equality within `tolerance`, for values that arrive through arithmetic rather than exactly.
export assertNear(double actual, double expected, double tolerance) throws TestFailure;

// Runs `body` and answers the E it threw; a body that throws nothing fails the test. The answer is
// the error itself, so the test goes on to check its kind or its message.
export assertThrows<E: Error>((() -> void throws E) body) -> E throws TestFailure;
```

## Decisions

`assertThrows` is generic over the error type and returns the caught error, because the expected condition cannot be a parameter: every module's `ErrorKind` is a different enum type.
The caller checks `kind` or `message` with the assertions that already exist.
This uses class-typed bounds (`E: Error`), decided in the language prerequisites.

The body's type declares `throws E`, so the thrown set at the call is exactly `E` and `assertThrows` catches it directly, with no runtime type test.
That moves the check to the call site: a test naming one error type and passing a body that raises another is a compile error rather than a failure at run time.
Two language decisions were made for this and hold everywhere, not just here (both ratified 2026-08-31): an explicit `throws` list is legal wherever `throws` is written, and a function type may carry one, written after the return type with the function type in parentheses.
A type parameter is also allowed where a class type is required for a runtime type test, which is `catch`, `is`, `as?`, and `switch` `is`-arms.

Structured diffs live in `assertEqual`'s implementation, not its signature.
`assertEqual` formats both values and hands the two strings to one plain formatter, so multi-line text reports the line that differs and long single-line text reports the offset, whatever the value's type was.
There is one equality assertion rather than a second name for sequences, because an array compares by content and formats as its contents (ratified 2026-08-31), which is the rule that also puts arrays and the collections above them under one description.

`assertEqual` keeps exact == on floats, which is sometimes right; `assertNear` is the tool for the rest.
No test may depend on any unspecified order: map iteration, set iteration, or sort tie order.
Each std module keeps the happy-path file and errors file split.
