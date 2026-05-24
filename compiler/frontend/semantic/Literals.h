#pragma once
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
// codepoint. Recognised escapes: \n \t \r \0 \\ \' \", any other \X falls
// back to the character X.
uint32_t parseCharLiteralCodepoint(std::u16string_view text);