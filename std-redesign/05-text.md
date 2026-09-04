# @std.text

The `string` primitive binding, `StringBuilder`, and number parsing.
`string` is guaranteed valid UTF-8 everywhere; offsets are byte offsets; iteration over characters is explicit through views.

## The string binding

```ens
// @std.text.string
// What text can do. These are the members of `string` itself; the compiler provides only length,
// byte access, `+`, and `==`, and everything here is ordinary Ens code over those.
primitive string implements Comparable<string> {
    export isEmpty() -> bool;

    // Search, by exact bytes. `indexOf` answers the byte offset of the first occurrence, or -1
    // when there is none; an empty needle is found at offset 0.
    export contains(string needle) -> bool;
    export indexOf(string needle) -> long;
    export indexOf(string needle, long from) -> long;
    export lastIndexOf(string needle) -> long;
    export startsWith(string prefix) -> bool;
    export endsWith(string suffix) -> bool;

    // The bytes in [start, end) as new text. Offsets are byte offsets; a range that is out of
    // bounds or cuts through the middle of a character aborts the program.
    export substring(long start, long end) -> string;
    export substring(long start) -> string;

    // Whitespace is a space or one of the ASCII layout controls: tab, line feed, vertical tab,
    // form feed, and carriage return.
    export trim() -> string;
    export trimStart() -> string;
    export trimEnd() -> string;

    export replace(string needle, string replacement) -> string;
    export repeat(long times) -> string;
    export padStart(long width, char filler) -> string;
    export padEnd(long width, char filler) -> string;

    // The parts between occurrences of `separator`, in order. Neighboring separators give an empty
    // part; text with no separator gives one part, the text itself; an empty separator does the
    // same, because there is nothing to split on.
    export split(string separator) -> string[];

    // The lines, split on '\n' with a trailing '\r' dropped from each, so both line endings read
    // the same. Text ending in a newline has a last, empty line.
    export lines() -> string[];

    // ASCII case only: 'A' through 'Z' and 'a' through 'z' convert, every other byte is kept. The
    // names say so because the full Unicode rules are a different operation with locale questions
    // this one refuses to guess at.
    export toLowerAscii() -> string;
    export toUpperAscii() -> string;
    export equalsIgnoreCaseAscii(string other) -> bool;

    // Byte order: negative when the receiver sorts first, zero on the same bytes, positive when it
    // sorts after. This equals code point order, which is not alphabetical order in any language
    // with accents.
    export override compareTo(string other) -> int;

    // The UTF-8 bytes, copied.
    export toBytes() -> byte[];

    // The characters and the raw bytes, walked without copying. Text does not iterate on its own:
    // a walk always names which view it reads.
    export chars() -> Iterable<char>;
    export bytes() -> Iterable<byte>;

    // Text from UTF-8 bytes. `fromBytes` refuses bytes that are not valid UTF-8; `fromBytesLossy`
    // replaces each invalid sequence with U+FFFD and never fails.
    export static fromBytes(byte[] bytes) -> string throws EncodingError;
    export static fromBytesLossy(byte[] bytes) -> string;

    // `parts` with `separator` between neighbors, nothing before the first or after the last.
    export static joined(string[] parts, string separator) -> string;
}
```

`EncodingError` lives in this module: one failure condition, so a `const long offset` field and no kind enum.

## StringBuilder

```ens
// @std.text.stringbuilder
// Accumulates text piece by piece in linear time, where `+` on immutable strings would copy the
// whole prefix on every append.
export final class StringBuilder {
    export constructor();
    export static withCapacity(long capacity) -> StringBuilder;

    export append(string value);
    export append(char value);
    export append(long value);
    export append(double value);
    export append(bool value);
    export appendLine(string value);
    export appendLine();

    // How many bytes the text built so far holds.
    export length() -> long;
    export isEmpty() -> bool;

    export clear();
    export reserve(long capacity);

    export toString() -> string;
}
```

## Parsing

```ens
// @std.text.parse
// Reading a value out of text. Every parser answers null for text that spells anything else:
// surrounding whitespace, a radix prefix, and digit grouping are refused rather than guessed at.
// Text that spells a number the type cannot hold is also null.

export parseLong(string text) -> long?;
export parseInt(string text) -> int?;
export parseLong(string text, int radix) -> long?;
export parseDouble(string text) -> double?;
export parseBool(string text) -> bool?;
```

## Decisions embodied here

`appendByte` is gone: it let callers build invalid UTF-8, and its one real user (Win32 quoting) is covered by `append(char)`.
Byte assembly belongs to a future `BytesBuilder` in `@std.io` if ever needed.
Parsers return `T?` and there is no `ParseError` class: nothing in the module throws, and "not a number" versus "too big" is a distinction almost no caller uses.
`fromBytes` throws rather than returning null because callers typically cannot proceed and the error carries the offending offset.
`toLowerAscii`, not `toLower`: a `toLower` that quietly handles only ASCII is a trap, and the honest name leaves room for a future Unicode operation with tables.
`chars()` and `bytes()` are non-copying views; `toBytes()` keeps the copying contract of `to`.
`joined` is a static on `string`, not a free function.
`string` implements `Comparable<string>` but not `Iterable<char>`; `for (let c in text)` does not compile.
`StringBuilder.append(double)` reuses the runtime's shortest round-trip formatting, which interpolation already has; whether a `float` gets the shortest text for a float rather than its double expansion is decided here.
Integer formatting in other bases (hex, binary, octal) with width and zero padding joins this surface (ratified 2026-09-04); the spelling is signed off before C6 writes it.
Case conversion beyond ASCII, locale collation, and Unicode normalization are all out of scope for v1.

## EncodingError

```ens
// @std.text
export class EncodingError extends Error {
    // The byte offset of the first invalid sequence.
    export const long offset;
    export constructor(this.message, this.offset, Error? cause = null);
}
```
