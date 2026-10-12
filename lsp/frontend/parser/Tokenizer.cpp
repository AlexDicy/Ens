#include "Tokenizer.h"

#include <utility>
#include "../diagnostics/Diagnostic.h"
#include "../diagnostics/DiagnosticSink.h"
#include "../semantic/Literals.h"

namespace {

bool isAsciiLetter(char16_t c) {
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z');
}

bool isDigit(char16_t c) {
    return c >= u'0' && c <= u'9';
}

bool isHexDigit(char16_t c) {
    return isDigit(c) || (c >= u'a' && c <= u'f') || (c >= u'A' && c <= u'F');
}

bool isIdentStart(char16_t c) {
    return isAsciiLetter(c) || c == u'_' || c == u'$';
}

bool isIdentContinue(char16_t c) {
    return isIdentStart(c) || isDigit(c);
}

bool isSimpleEscape(char16_t c) {
    return c == u'\\' || c == u'"' || c == u'\'' || c == u'n' || c == u'r' ||
           c == u't' || c == u'b' || c == u'f' || c == u'0' || c == u'{' || c == u'}';
}

// A character literal holds exactly one character: one escape or one code point, counted over the
// literal's UTF-8 bytes.

bool isHexByte(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool isContinuationByte(unsigned char c) {
    return c >= 0x80 && c <= 0xBF;
}

size_t utf8SequenceLength(unsigned char lead) {
    if (lead < 0x80) return 1;
    if (lead >= 0xF0 && lead <= 0xF7) return 4;
    if (lead >= 0xE0 && lead <= 0xEF) return 3;
    if (lead >= 0xC0 && lead <= 0xDF) return 2;
    return 1;
}

size_t escapeLength(const std::string& text, size_t index) {
    if (index + 5 < text.size() && text[index + 1] == 'u' &&
        isHexByte(text[index + 2]) && isHexByte(text[index + 3]) &&
        isHexByte(text[index + 4]) && isHexByte(text[index + 5])) {
        return 6;
    }
    return 2;
}

size_t characterBytes(const std::string& text, size_t index) {
    unsigned char lead = text[index];
    if (lead == '\\') return escapeLength(text, index);
    size_t length = utf8SequenceLength(lead);
    return index + length > text.size() ? text.size() - index : length;
}

long hexValue(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

long codePointAt(const std::string& text, size_t index, size_t length) {
    unsigned char lead = text[index];
    if (lead == '\\' && length == 6) {
        long value = 0;
        for (size_t digit = index + 2; digit < index + 6; ++digit) {
            value = value * 16 + hexValue(text[digit]);
        }
        return value;
    }
    if (lead == '\\') return static_cast<unsigned char>(text[index + 1]);
    if (length == 1) return lead;
    long leadBits = length == 2 ? 0x1F : length == 3 ? 0x0F : 0x07;
    long value = lead & leadBits;
    for (size_t next = index + 1; next < index + length; ++next) {
        value = value * 64 + (static_cast<unsigned char>(text[next]) & 0x3F);
    }
    return value;
}

bool joinsPrevious(long codePoint) {
    return (codePoint >= 0x0300 && codePoint <= 0x036F) ||
           (codePoint >= 0x1AB0 && codePoint <= 0x1AFF) ||
           (codePoint >= 0x1DC0 && codePoint <= 0x1DFF) ||
           (codePoint >= 0x20D0 && codePoint <= 0x20FF) ||
           (codePoint >= 0xFE20 && codePoint <= 0xFE2F) || codePoint == 0x200D ||
           (codePoint >= 0xFE00 && codePoint <= 0xFE0F);
}

// The characters between the quotes of a character literal, where an escape counts as one and so
// does each code point.
long characterLiteralLength(const std::string& literal) {
    long count = 0;
    size_t index = 1;
    while (index + 1 < literal.size()) {
        unsigned char c = literal[index];
        if (c == '\\') {
            count++;
            index += escapeLength(literal, index);
            continue;
        }
        if (!isContinuationByte(c)) count++;
        index++;
    }
    return count;
}

bool hasJoiningMark(const std::string& content) {
    size_t index = 0;
    bool first = true;
    while (index < content.size()) {
        if (isContinuationByte(content[index])) {
            index++;
            continue;
        }
        size_t length = characterBytes(content, index);
        if (!first && joinsPrevious(codePointAt(content, index, length))) return true;
        first = false;
        index += length;
    }
    return false;
}

// What a string literal writes to hold the characters of a character literal's content.
std::string stringContent(const std::string& content) {
    std::string written;
    size_t runStart = 0;
    size_t index = 0;
    while (index < content.size()) {
        char c = content[index];
        if (c == '\\') {
            index += escapeLength(content, index);
            continue;
        }
        if (c == '"' || c == '{' || c == '}') {
            written += content.substr(runStart, index - runStart) + "\\";
            runStart = index;
        }
        index++;
    }
    return written + content.substr(runStart);
}

std::string characterLengthRefusal(const std::string& literal) {
    long count = characterLiteralLength(literal);
    std::string content = literal.substr(1, literal.size() - 2);
    if (count == 0) {
        return "'' has no character between its quotes, but a character literal must have exactly "
               "one. Write the character between the quotes, as in 'a', or write '\"\"' for an "
               "empty string.";
    }
    std::string text = "'\"" + stringContent(content) + "\"'";
    if (hasJoiningMark(content)) {
        return literal + " is made of " + std::to_string(count) +
               " Unicode code points, but a character literal must have exactly one. A code "
               "point is one numbered entry in Unicode, and some characters, such as a letter "
               "with a separate accent mark, take more than one. Write " + text +
               " to make it a string.";
    }
    return literal + " has " + std::to_string(count) +
           " characters, but a character literal must have exactly one. Write " + text +
           " to make it a string, or keep one character, as in '" +
           content.substr(0, characterBytes(content, 0)) + "'.";
}

}  // namespace

Tokenizer::Tokenizer(std::u16string_view src, DiagnosticSink& s)
    : source(src), sink(s) {}

bool Tokenizer::atEnd() const {
    return pos >= source.size();
}

char16_t Tokenizer::peek(uint32_t offset) const {
    uint32_t idx = pos + offset;
    return idx < source.size() ? source[idx] : u'\0';
}

void Tokenizer::advance() {
    if (pos >= source.size()) return;
    char16_t c = source[pos];
    pos++;
    if (c == u'\n') { line++; column = 1; }
    else if (c == u'\r') {
        if (pos < source.size() && source[pos] == u'\n') {
            pos++;
        }
        line++;
        column = 1;
    } else {
        column++;
    }
}

LexedToken Tokenizer::makeToken(SyntaxKind kind, uint32_t startPos, int startLine, int startCol) {
    LexedToken t;
    t.kind = kind;
    t.text = std::u16string(source.substr(startPos, pos - startPos));
    t.offset = startPos;
    t.line = startLine;
    t.column = startCol;
    return t;
}

LexedToken Tokenizer::next() {
    if (atEnd()) {
        LexedToken t;
        t.kind = SyntaxKind::EndOfFile;
        t.offset = pos;
        t.line = line;
        t.column = column;
        return t;
    }

    uint32_t startPos = pos;
    int startLine = line;
    int startCol = column;
    char16_t c = peek();

    // Inside an interpolation hole, a `}` at brace depth 0 closes the hole and
    // resumes string lexing; `{`/`}` at deeper levels are ordinary operators
    // whose nesting we track so the closing brace is not mistaken for one.
    if (!interpHoleDepth.empty()) {
        if (c == u'}' && interpHoleDepth.back() == 0) {
            advance();  // consume the hole's closing '}'
            return lexStringBody(startPos, startLine, startCol, /*atStart=*/false);
        }
        if (c == u'{') interpHoleDepth.back()++;
        else if (c == u'}') interpHoleDepth.back()--;
    }

    if (c == u' ' || c == u'\t' || c == u'\f' || c == 0xB) {
        return lexWhitespace(startPos, startLine, startCol);
    }
    if (c == u'\n' || c == u'\r') {
        return lexNewline(startPos, startLine, startCol);
    }
    if (c == u'/') {
        char16_t next = peek(1);
        if (next == u'/') return lexLineComment(startPos, startLine, startCol);
        if (next == u'*') return lexBlockComment(startPos, startLine, startCol);
        return lexOperator(startPos, startLine, startCol);
    }
    if (isDigit(c)) {
        return lexNumber(startPos, startLine, startCol);
    }
    if (isIdentStart(c)) {
        return lexIdentifierOrKeyword(startPos, startLine, startCol);
    }
    if (c == u'"') {
        return lexString(startPos, startLine, startCol);
    }
    if (c == u'\'') {
        return lexChar(startPos, startLine, startCol);
    }
    return lexOperator(startPos, startLine, startCol);
}

LexedToken Tokenizer::lexWhitespace(uint32_t startPos, int startLine, int startCol) {
    while (!atEnd()) {
        char16_t c = peek();
        if (c == u' ' || c == u'\t' || c == u'\f' || c == 0xB) advance();
        else break;
    }
    return makeToken(SyntaxKind::Whitespace, startPos, startLine, startCol);
}

LexedToken Tokenizer::lexNewline(uint32_t startPos, int startLine, int startCol) {
    advance();  // consume one line terminator (advance() handles CRLF as a unit)
    return makeToken(SyntaxKind::Newline, startPos, startLine, startCol);
}

LexedToken Tokenizer::lexLineComment(uint32_t startPos, int startLine, int startCol) {
    advance(); advance();  // //
    while (!atEnd()) {
        char16_t c = peek();
        if (c == u'\n' || c == u'\r') break;
        advance();
    }
    return makeToken(SyntaxKind::LineComment, startPos, startLine, startCol);
}

LexedToken Tokenizer::lexBlockComment(uint32_t startPos, int startLine, int startCol) {
    advance(); advance();  // /*
    bool closed = false;
    while (!atEnd()) {
        char16_t c = peek();
        if (c == u'*' && peek(1) == u'/') {
            advance(); advance();
            closed = true;
            break;
        }
        advance();
    }
    if (!closed) {
        sink.error({startLine, startCol, static_cast<int>(pos - startPos)},
                   "Unterminated block comment");
    }
    return makeToken(SyntaxKind::BlockComment, startPos, startLine, startCol);
}

LexedToken Tokenizer::lexIdentifierOrKeyword(uint32_t startPos, int startLine, int startCol) {
    while (!atEnd() && isIdentContinue(peek())) advance();
    std::u16string text(source.substr(startPos, pos - startPos));
    SyntaxKind kind = keywordKindFromText(text);
    LexedToken t;
    t.kind = kind;
    t.text = std::move(text);
    t.offset = startPos;
    t.line = startLine;
    t.column = startCol;
    return t;
}

LexedToken Tokenizer::lexNumber(uint32_t startPos, int startLine, int startCol) {
    bool isFloat = false;
    bool isLong = false;
    bool isFloatSuffix = false;

    if (peek() == u'0' && (peek(1) == u'x' || peek(1) == u'X')) {
        advance(); advance();
        bool any = false;
        while (!atEnd() && (isHexDigit(peek()) || peek() == u'_')) { any = true; advance(); }
        if (!any) sink.error({startLine, startCol, static_cast<int>(pos - startPos)}, "Invalid hex number");
    } else if (peek() == u'0' && (peek(1) == u'b' || peek(1) == u'B')) {
        advance(); advance();
        bool any = false;
        while (!atEnd() && (peek() == u'0' || peek() == u'1' || peek() == u'_')) { any = true; advance(); }
        if (!any) sink.error({startLine, startCol, static_cast<int>(pos - startPos)}, "Invalid binary number");
    } else {
        while (!atEnd() && (isDigit(peek()) || peek() == u'_')) advance();
        if (peek() == u'.' && isDigit(peek(1))) {
            isFloat = true;
            advance();
            while (!atEnd() && (isDigit(peek()) || peek() == u'_')) advance();
        }
        if (peek() == u'e' || peek() == u'E') {
            isFloat = true;
            advance();
            if (peek() == u'+' || peek() == u'-') advance();
            while (!atEnd() && isDigit(peek())) advance();
        }
    }

    if (peek() == u'f' || peek() == u'F') {
        isFloat = true;
        isFloatSuffix = true;
        advance();
    } else if (peek() == u'd' || peek() == u'D') {
        isFloat = true;
        advance();
    } else if (peek() == u'l' || peek() == u'L') {
        isLong = true;
        advance();
    }

    SyntaxKind kind;
    if (isFloatSuffix) kind = SyntaxKind::FloatLiteral;
    else if (isFloat) kind = SyntaxKind::DoubleLiteral;
    else if (isLong) kind = SyntaxKind::LongLiteral;
    else kind = SyntaxKind::IntLiteral;
    return makeToken(kind, startPos, startLine, startCol);
}

LexedToken Tokenizer::lexString(uint32_t startPos, int startLine, int startCol) {
    advance();  // opening "
    return lexStringBody(startPos, startLine, startCol, /*atStart=*/true);
}

LexedToken Tokenizer::lexStringBody(uint32_t startPos, int startLine, int startCol, bool atStart) {
    bool closed = false;
    bool hole = false;
    while (!atEnd()) {
        char16_t c = peek();
        if (c == u'"') { advance(); closed = true; break; }
        if (c == u'{') { advance(); hole = true; break; }
        if (c == u'\n' || c == u'\r') break;
        if (c == u'\\') {
            consumeEscape();
            continue;
        }
        advance();
    }
    if (!closed && !hole) {
        sink.error({startLine, startCol, static_cast<int>(pos - startPos)},
                   "Unterminated string literal");
    }
    if (atStart && !hole) return makeToken(SyntaxKind::StringLiteral, startPos, startLine, startCol);
    if (atStart) {  // hole
        interpHoleDepth.push_back(0);
        return makeToken(SyntaxKind::InterpStringStart, startPos, startLine, startCol);
    }
    if (hole) return makeToken(SyntaxKind::InterpStringMid, startPos, startLine, startCol);
    if (!interpHoleDepth.empty()) interpHoleDepth.pop_back();
    return makeToken(SyntaxKind::InterpStringEnd, startPos, startLine, startCol);
}

LexedToken Tokenizer::lexChar(uint32_t startPos, int startLine, int startCol) {
    advance();  // opening '
    bool closed = false;
    size_t reportedBefore = sink.list().size();
    while (!atEnd()) {
        char16_t c = peek();
        if (c == u'\'') { advance(); closed = true; break; }
        if (c == u'\n' || c == u'\r') break;
        if (c == u'\\') {
            consumeEscape();
            continue;
        }
        advance();
    }
    if (!closed) {
        sink.error({startLine, startCol, static_cast<int>(pos - startPos)},
                   "Unterminated character literal");
    } else if (sink.list().size() == reportedBefore) {
        std::string literal = utf16ToUtf8(source.substr(startPos, pos - startPos));
        if (characterLiteralLength(literal) != 1) {
            sink.error({startLine, startCol, static_cast<int>(pos - startPos)},
                       characterLengthRefusal(literal));
        }
    }
    return makeToken(SyntaxKind::CharLiteral, startPos, startLine, startCol);
}

void Tokenizer::consumeEscape() {
    uint32_t escapeStartPos = pos;
    int escapeStartLine = line;
    int escapeStartCol = column;
    advance();  // backslash
    if (atEnd()) return;
    char16_t escaped = peek();
    if (isSimpleEscape(escaped)) {
        advance();
        return;
    }
    auto spliced = [&] {
        return utf16ToUtf8(source.substr(escapeStartPos, pos - escapeStartPos));
    };
    if (escaped == u'u') {
        advance();
        if (isHexDigit(peek(0)) && isHexDigit(peek(1)) &&
            isHexDigit(peek(2)) && isHexDigit(peek(3))) {
            advance(); advance(); advance(); advance();
            return;
        }
        sink.error({escapeStartLine, escapeStartCol, static_cast<int>(pos - escapeStartPos)},
                   "Invalid Unicode escape sequence '" + spliced() +
                       "': write four hex digits, for example \\u00E9");
        return;
    }
    advance();
    sink.error({escapeStartLine, escapeStartCol, static_cast<int>(pos - escapeStartPos)},
               "Invalid escape sequence '" + spliced() +
                   "': valid escapes include \\n, \\t, and \\\\");
}

LexedToken Tokenizer::lexOperator(uint32_t startPos, int startLine, int startCol) {
    char16_t c0 = peek(0);
    char16_t c1 = peek(1);
    char16_t c2 = peek(2);
    char16_t c3 = peek(3);

    auto consume = [&](int n, SyntaxKind k) {
        for (int i = 0; i < n; ++i) advance();
        return makeToken(k, startPos, startLine, startCol);
    };

    // 4-char
    if (c0 == u'>' && c1 == u'>' && c2 == u'>' && c3 == u'=') return consume(4, SyntaxKind::GtGtGtEq);

    // 3-char
    if (c0 == u'>' && c1 == u'>' && c2 == u'>')               return consume(3, SyntaxKind::GtGtGt);
    if (c0 == u'>' && c1 == u'>' && c2 == u'=')               return consume(3, SyntaxKind::GtGtEq);
    if (c0 == u'<' && c1 == u'<' && c2 == u'=')               return consume(3, SyntaxKind::LtLtEq);
    if (c0 == u'.' && c1 == u'.' && c2 == u'.')               return consume(3, SyntaxKind::Ellipsis);

    // 2-char
    if (c0 == u'=' && c1 == u'=') return consume(2, SyntaxKind::EqEq);
    if (c0 == u'!' && c1 == u'=') return consume(2, SyntaxKind::NotEq);
    if (c0 == u'<' && c1 == u'=') return consume(2, SyntaxKind::LtEq);
    if (c0 == u'>' && c1 == u'=') return consume(2, SyntaxKind::GtEq);
    if (c0 == u'-' && c1 == u'>') return consume(2, SyntaxKind::Arrow);
    if (c0 == u'&' && c1 == u'&') return consume(2, SyntaxKind::AmpAmp);
    if (c0 == u'|' && c1 == u'|') return consume(2, SyntaxKind::PipePipe);
    if (c0 == u'+' && c1 == u'+') return consume(2, SyntaxKind::PlusPlus);
    if (c0 == u'-' && c1 == u'-') return consume(2, SyntaxKind::MinusMinus);
    if (c0 == u'<' && c1 == u'<') return consume(2, SyntaxKind::LtLt);
    if (c0 == u'>' && c1 == u'>') return consume(2, SyntaxKind::GtGt);
    if (c0 == u'+' && c1 == u'=') return consume(2, SyntaxKind::PlusEq);
    if (c0 == u'-' && c1 == u'=') return consume(2, SyntaxKind::MinusEq);
    if (c0 == u'*' && c1 == u'=') return consume(2, SyntaxKind::StarEq);
    if (c0 == u'/' && c1 == u'=') return consume(2, SyntaxKind::SlashEq);
    if (c0 == u'%' && c1 == u'=') return consume(2, SyntaxKind::PercentEq);
    if (c0 == u'&' && c1 == u'=') return consume(2, SyntaxKind::AmpEq);
    if (c0 == u'|' && c1 == u'=') return consume(2, SyntaxKind::PipeEq);
    if (c0 == u'^' && c1 == u'=') return consume(2, SyntaxKind::CaretEq);
    if (c0 == u'?' && c1 == u'.') return consume(2, SyntaxKind::QuestionDot);

    // 1-char
    switch (c0) {
        case u'(': return consume(1, SyntaxKind::LParen);
        case u')': return consume(1, SyntaxKind::RParen);
        case u'{': return consume(1, SyntaxKind::LBrace);
        case u'}': return consume(1, SyntaxKind::RBrace);
        case u'[': return consume(1, SyntaxKind::LBracket);
        case u']': return consume(1, SyntaxKind::RBracket);
        case u';': return consume(1, SyntaxKind::Semi);
        case u',': return consume(1, SyntaxKind::Comma);
        case u'.': return consume(1, SyntaxKind::Dot);
        case u':': return consume(1, SyntaxKind::Colon);
        case u'+': return consume(1, SyntaxKind::Plus);
        case u'-': return consume(1, SyntaxKind::Minus);
        case u'*': return consume(1, SyntaxKind::Star);
        case u'/': return consume(1, SyntaxKind::Slash);
        case u'%': return consume(1, SyntaxKind::Percent);
        case u'^': return consume(1, SyntaxKind::Caret);
        case u'=': return consume(1, SyntaxKind::Eq);
        case u'!': return consume(1, SyntaxKind::Bang);
        case u'~': return consume(1, SyntaxKind::Tilde);
        case u'?': return consume(1, SyntaxKind::Question);
        case u'<': return consume(1, SyntaxKind::Lt);
        case u'>': return consume(1, SyntaxKind::Gt);
        case u'&': return consume(1, SyntaxKind::Amp);
        case u'|': return consume(1, SyntaxKind::Pipe);
        case u'@': return consume(1, SyntaxKind::At);
    }

    sink.error({startLine, startCol, 1},
               std::string("Unexpected character '") +
               (c0 < 128 ? static_cast<char>(c0) : '?') + "'");
    advance();
    return makeToken(SyntaxKind::Invalid, startPos, startLine, startCol);
}
