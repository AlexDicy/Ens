#pragma once
#include <cstdint>
#include <string>
#include <string_view>
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
    LexedToken lexChar(uint32_t startPos, int startLine, int startCol);
    LexedToken lexOperator(uint32_t startPos, int startLine, int startCol);
};
