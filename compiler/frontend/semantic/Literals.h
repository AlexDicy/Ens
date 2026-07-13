#pragma once
#include <cstddef>
#include <cstdint>
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