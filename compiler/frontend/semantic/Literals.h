#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Parses the text of an `IntLiteral` token (decimal, `0x...`, `0b...`, with
// optional `_` separators and trailing `l`/`L` type suffix). The text does
// NOT include a leading `+`/`-`, the parser places those as a separate
// `PrefixExpression` over the literal.
//
// Returns true on success and writes the unsigned magnitude into `out`.
// Returns false if the text is malformed or the magnitude overflows 64 bits.
bool parseIntegerLiteralMagnitude(std::u16string_view text, uint64_t& out);

// Decodes a `CharLiteral` token's text (still surrounded by single quotes,
// possibly containing an escape sequence or a surrogate pair) into a Unicode
// codepoint. Recognised escapes: \n \t \r \b \f \0 \\ \' \" \uXXXX, any other
// \X falls back to the character X.
uint32_t parseCharLiteralCodepoint(std::u16string_view text);

// Decodes the backslash escape whose backslash sits at text[backslashIndex]
// (the caller guarantees text[backslashIndex] == '\\' and backslashIndex + 1 <
// end). Returns the decoded Unicode scalar value and writes the index of the
// first character past the escape into `next`. A '\uXXXX' escape consumes four
// hex digits; a recognized letter maps to its control character; anything else,
// including a malformed '\u', yields the character following the backslash.
// This is the single source of truth for the escape set shared by char
// literals, string literals, and interpolated segments.
uint32_t decodeEscapeSequence(std::u16string_view text, size_t backslashIndex,
                              size_t end, size_t& next);

// Decodes a `StringLiteral` token's text (still surrounded by double quotes,
// possibly containing escape sequences) into its value, so two spellings of the
// same string compare equal. Escapes follow the shared set of decodeEscapeSequence;
// astral scalars are re-encoded as UTF-16 surrogate pairs.
std::u16string decodeStringLiteral(std::u16string_view text);

// Appends the UTF-8 encoding (1 to 4 bytes) of a Unicode scalar to `out`.
// Values in the surrogate range or above U+10FFFF are still encoded byte-wise;
// callers decoding from UTF-16 recombine surrogate pairs before calling this.
void appendUtf8(std::string& out, uint32_t scalar);

// Re-encodes a UTF-16 string as UTF-8, recombining surrogate pairs into their
// 4-byte forms. This is the single source of truth for UTF-16 to UTF-8.
std::string utf16ToUtf8(std::u16string_view s);

// Describes where UTF-8 decoding first failed. `reason` points at a static
// string literal, so the error outlives the decoded buffer.
struct Utf8DecodeError {
    size_t byteOffset = 0;   // offset of the offending byte in the input
    size_t line = 1;         // 1-based line of the offending byte
    unsigned char byte = 0;  // value of the offending byte
    const char* reason = "";
};

// Decodes UTF-8 `bytes` into UTF-16 `out` (surrogate pairs for astral scalars),
// skipping a leading UTF-8 BOM if present. On success returns true. On the
// first ill-formed sequence returns false, fills `err`, and leaves `out`
// holding the code units decoded so far.
bool decodeUtf8ToUtf16(std::string_view bytes, std::u16string& out, Utf8DecodeError& err);

// Formats a beginner-friendly message for a UTF-8 decode failure, naming the
// offending byte, its line, and how to fix it. The caller prefixes the file.
std::string describeUtf8DecodeError(const Utf8DecodeError& err);