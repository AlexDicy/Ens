#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "../cst/SyntaxKind.h"

class DiagnosticSink;

struct LexedToken {
    SyntaxKind kind;
    std::u16string text;
    uint32_t offset;
    int line;
    int column;
};

class Tokenizer {
public:
    Tokenizer(std::u16string_view source, DiagnosticSink& sink);

    LexedToken next();
    bool atEnd() const;

private:
    std::u16string_view source;
    DiagnosticSink& sink;
    uint32_t pos = 0;
    int line = 1;
    int column = 1;
    // One entry per open interpolation hole, holding the brace nesting inside
    // that hole's expression. The current hole closes on a `}` at depth 0.
    std::vector<int> interpHoleDepth;

    char16_t peek(uint32_t offset = 0) const;
    void advance();

    LexedToken makeToken(SyntaxKind kind, uint32_t startPos, int startLine, int startCol);

    LexedToken lexWhitespace(uint32_t startPos, int startLine, int startCol);
    LexedToken lexNewline(uint32_t startPos, int startLine, int startCol);
    LexedToken lexLineComment(uint32_t startPos, int startLine, int startCol);
    LexedToken lexBlockComment(uint32_t startPos, int startLine, int startCol);
    LexedToken lexIdentifierOrKeyword(uint32_t startPos, int startLine, int startCol);
    LexedToken lexNumber(uint32_t startPos, int startLine, int startCol);
    LexedToken lexString(uint32_t startPos, int startLine, int startCol);
    // Scans one string segment from the current position to the next unescaped
    // `{` (a hole opens) or the closing `"`. `atStart` is true just after the
    // opening quote, false just after a hole's closing `}`.
    LexedToken lexStringBody(uint32_t startPos, int startLine, int startCol, bool atStart);
    LexedToken lexChar(uint32_t startPos, int startLine, int startCol);
    LexedToken lexOperator(uint32_t startPos, int startLine, int startCol);

    // Consumes a backslash escape (the backslash plus its target, and the four
    // hex digits of a `\uXXXX`) and reports an unknown escape or a malformed
    // `\uXXXX` over the offending span. Shared by string, char, and
    // interpolated-segment lexing.
    void consumeEscape();
};
