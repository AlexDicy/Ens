#include <iostream>
#include <memory>
#include "Tokenizer.h"
#include "../util/UnicodeScanner.h"
#include "../diagnostics/Diagnostic.h"


std::vector<Token> Tokenizer::tokenize(std::u16string_view code) {
    std::vector<Token> tokens;
    if (!code.empty()) {
        auto sc = std::make_unique<UnicodeScanner>(code);
        Tokenizer tokenizer;
        tokenizer.scanner = sc.get();

        auto token = tokenizer.readToken();
        while (token.has_value() && token->getType() != TokenType::END_OF_FILE) {
            tokens.push_back(*token);
            token = tokenizer.readToken();
        }
        for (const auto& t : tokens) {
            const auto& wide = t.getText();
            std::string text(wide.size(), 0);
            for (size_t i = 0; i < wide.size(); ++i) text[i] = (char)wide[i];
            std::cout << '>' << (int)t.getType() << " - " << text << '\n';
        }
    }
    return tokens;
}

std::optional<Token> Tokenizer::readToken() {
    scanner->spointer = 0;
    int pos = scanner->pointer;
    tokenType = std::nullopt;
    bool loop = true;
    line = scanner->line;
    col = scanner->column;

    while (loop) {
        line = scanner->line;
        col = scanner->column;
        switch (scanner->c) {
            case ' ':
            case '\t':
            case UnicodeScanner::FF:
                scanner->scanChar();
                while (scanner->c == ' ' || scanner->c == '\t') {
                    scanner->scanChar();
                }
                break;
            case UnicodeScanner::LF:
                scanner->scanChar();
                break;
            case UnicodeScanner::CR:
                scanner->scanChar();
                if (scanner->c == UnicodeScanner::LF) {
                    scanner->scanChar();
                }
                break;
            case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
            case 'G': case 'H': case 'I': case 'J': case 'K': case 'L':
            case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R':
            case 'S': case 'T': case 'U': case 'V': case 'W': case 'X':
            case 'Y': case 'Z':
            case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
            case 'g': case 'h': case 'i': case 'j': case 'k': case 'l':
            case 'm': case 'n': case 'o': case 'p': case 'q': case 'r':
            case 's': case 't': case 'u': case 'v': case 'w': case 'x':
            case 'y': case 'z':
            case '$': case '_':
                scanIdentifier();
                loop = false;
                break;
            case '0':
                scanner->scanChar();
                if (scanner->c == 'x' || scanner->c == 'X') {
                    scanner->putChar(u'0');
                    scanner->putChar(true);
                    scanNumber(pos, 16);
                } else if (scanner->c == 'b' || scanner->c == 'B') {
                    scanner->putChar(u'0');
                    scanner->putChar(true);
                    scanNumber(pos, 2);
                } else {
                    scanner->putChar(u'0');
                    if (scanner->c == '_') {
                        do {
                            scanner->scanChar();
                        } while (scanner->c == '_');
                        if (scanner->digit(pos, 10) < 0) {
                            error("Illegal Underscore");
                        }
                    }
                    scanNumber(pos, 8);
                }
                loop = false;
                break;
            case '1': case '2': case '3':
            case '4': case '5': case '6':
            case '7': case '8': case '9':
                scanNumber(pos, 10);
                loop = false;
                break;
            case '.':
                scanner->scanChar();
                if (scanner->digit(pos, 10) >= 0) {
                    scanner->putChar(u'.');
                    scanFractionAndSuffix(pos);
                } else if (scanner->c == '.') {
                    scanner->putChar(u'.');
                    scanner->putChar(u'.', true);
                    if (scanner->c == '.') {
                        scanner->scanChar();
                        scanner->putChar(u'.');
                        tokenType = TokenType::ELLIPSIS;
                    } else {
                        error("Illegal Dot");
                    }
                } else {
                    tokenType = TokenType::DOT;
                }
                loop = false;
                break;
            case ',':
                scanner->scanChar();
                tokenType = TokenType::COMMA;
                loop = false;
                break;
            case ';':
                scanner->scanChar();
                tokenType = TokenType::SEMI;
                loop = false;
                break;
            case '(':
                scanner->scanChar();
                tokenType = TokenType::L_PAREN;
                loop = false;
                break;
            case ')':
                scanner->scanChar();
                tokenType = TokenType::R_PAREN;
                loop = false;
                break;
            case '[':
                scanner->scanChar();
                tokenType = TokenType::L_BRACKET;
                loop = false;
                break;
            case ']':
                scanner->scanChar();
                tokenType = TokenType::R_BRACKET;
                loop = false;
                break;
            case '{':
                scanner->scanChar();
                tokenType = TokenType::L_BRACE;
                loop = false;
                break;
            case '}':
                scanner->scanChar();
                tokenType = TokenType::R_BRACE;
                loop = false;
                break;
            case '/':
                scanner->scanChar();
                if (scanner->c == '/') {
                    do {
                        scanner->scanCommentChar();
                    } while (scanner->c != UnicodeScanner::CR && scanner->c != UnicodeScanner::LF
                             && scanner->pointer < (int)scanner->buffer.size());
                    break;
                } else if (scanner->c == '*') {
                    scanner->scanChar();
                    while (scanner->pointer < (int)scanner->buffer.size()) {
                        if (scanner->c == '*') {
                            scanner->scanChar();
                            if (scanner->c == '/') break;
                        } else {
                            scanner->scanCommentChar();
                        }
                    }
                    if (scanner->c == '/') {
                        scanner->scanChar();
                    } else {
                        error("Unclosed Comment");
                        loop = false;
                    }
                    break;
                } else if (scanner->c == '=') {
                    tokenType = TokenType::SLASH_EQ;
                    scanner->scanChar();
                } else {
                    tokenType = TokenType::SLASH;
                }
                loop = false;
                break;
            case '\'':
                scanner->scanChar();
                if (scanner->c == '\'') {
                    error("Empty CharLit");
                    scanner->scanChar();
                } else {
                    if (scanner->c == UnicodeScanner::LF || scanner->c == UnicodeScanner::CR)
                        error("Illegal LineEnd In CharLit");
                    scanLitChar(pos, true, false);
                    if (scanner->c == '\'') {
                        scanner->scanChar();
                        tokenType = TokenType::CHAR_LITERAL;
                    } else {
                        error("Unclosed CharLit");
                    }
                }
                loop = false;
                break;
            case '"':
                scanString(pos);
                loop = false;
                break;
            default:
                if (isSpecial(scanner->c)) {
                    scanOperator();
                } else {
                    if (scanner->pointer == (int)scanner->buffer.size()
                        || (scanner->c == UnicodeScanner::EOI
                            && scanner->pointer + 1 == (int)scanner->buffer.size())) {
                        tokenType = TokenType::END_OF_FILE;
                        pos = scanner->realLength;
                    }
                }
                loop = false;
        }
    }

    if (!tokenType.has_value()) return std::nullopt;
    return Token(*tokenType, scanner->getSaved(), line, col);
}

void Tokenizer::error(const char* msg) {
    int len = 1;
    if (scanner->line == line && scanner->column > col) {
        len = scanner->column - col;
    }
    SourceSpan span{line, col, len};
    throw Diagnostic(DiagnosticLevel::Error, span, msg);
}

bool Tokenizer::isSpecial(char16_t ch) {
    switch (ch) {
        case '!': case '%': case '&': case '*':
        case '?': case '+': case '-': case ':':
        case '<': case '=': case '>': case '^':
        case '|': case '~': case '@':
            return true;
        default:
            return false;
    }
}

void Tokenizer::scanOperator() {
    while (true) {
        scanner->putChar(false);
        TokenType tk = lookupToken(scanner->getSaved());
        if (tk == TokenType::IDENTIFIER) {
            scanner->spointer--;
            break;
        }
        tokenType = tk;
        scanner->scanChar();
        if (!isSpecial(scanner->c)) break;
    }
}

void Tokenizer::scanIdentifier() {
    scanner->putChar(true);
    while (true) {
        switch (scanner->c) {
            case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
            case 'G': case 'H': case 'I': case 'J': case 'K': case 'L':
            case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R':
            case 'S': case 'T': case 'U': case 'V': case 'W': case 'X':
            case 'Y': case 'Z':
            case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
            case 'g': case 'h': case 'i': case 'j': case 'k': case 'l':
            case 'm': case 'n': case 'o': case 'p': case 'q': case 'r':
            case 's': case 't': case 'u': case 'v': case 'w': case 'x':
            case 'y': case 'z':
            case '$': case '_':
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                scanner->putChar(true);
                break;
            default:
                tokenType = lookupToken(scanner->getSaved());
                return;
        }
    }
}

void Tokenizer::scanNumber(int pos, int radix) {
    this->radix = radix;
    int digitRadix = (radix == 8 ? 10 : radix);
    int firstDigit = scanner->digit(pos, (10 > digitRadix ? 10 : digitRadix));
    bool seendigit = firstDigit >= 0;
    bool seenValidDigit = firstDigit >= 0 && firstDigit < digitRadix;
    if (seendigit) {
        scanDigits(pos, digitRadix);
    }
    if (radix == 16 && scanner->c == '.') {
        scanHexFractionAndSuffix(pos, seendigit);
    } else if (seendigit && radix == 16 && (scanner->c == 'p' || scanner->c == 'P')) {
        scanHexExponentAndSuffix(pos);
    } else if (digitRadix == 10 && scanner->c == '.') {
        scanner->putChar(true);
        scanFractionAndSuffix(pos);
    } else if (digitRadix == 10
               && (scanner->c == 'e' || scanner->c == 'E'
                   || scanner->c == 'f' || scanner->c == 'F'
                   || scanner->c == 'd' || scanner->c == 'D')) {
        scanFractionAndSuffix(pos);
    } else {
        if (!seenValidDigit) {
            switch (radix) {
                case 2:  error("Invalid Binary Number"); break;
                case 16: error("Invalid Hex Number");    break;
            }
        }
        if (scanner->c == 'l' || scanner->c == 'L') {
            scanner->scanChar();
            tokenType = TokenType::LONG_LITERAL;
        } else {
            tokenType = TokenType::INT_LITERAL;
        }
    }
}

void Tokenizer::scanDigits(int pos, int digitRadix) {
    char16_t saveCh;
    do {
        if (scanner->c != '_') {
            scanner->putChar(false);
        }
        saveCh = scanner->c;
        scanner->scanChar();
    } while (scanner->digit(pos, digitRadix) >= 0 || scanner->c == '_');
    if (saveCh == '_')
        error("Illegal Underscore");
}

void Tokenizer::scanHexExponentAndSuffix(int pos) {
    if (scanner->c == 'p' || scanner->c == 'P') {
        scanner->putChar(true);
        if (scanner->c == '+' || scanner->c == '-') {
            scanner->putChar(true);
        }
        if (scanner->digit(pos, 10) >= 0) {
            scanDigits(pos, 10);
        } else {
            error("MalformedFpLit");
        }
    } else {
        error("MalformedFpLit");
    }
    if (scanner->c == 'f' || scanner->c == 'F') {
        scanner->putChar(true);
        tokenType = TokenType::FLOAT_LITERAL;
        this->radix = 16;
    } else {
        if (scanner->c == 'd' || scanner->c == 'D') {
            scanner->putChar(true);
        }
        tokenType = TokenType::DOUBLE_LITERAL;
        this->radix = 16;
    }
}

void Tokenizer::scanFraction(int pos) {
    if (scanner->digit(pos, 10) >= 0) {
        scanDigits(pos, 10);
    }
    int sp1 = scanner->spointer;
    if (scanner->c == 'e' || scanner->c == 'E') {
        scanner->putChar(true);
        if (scanner->c == '+' || scanner->c == '-') {
            scanner->putChar(true);
        }
        if (scanner->digit(pos, 10) >= 0) {
            scanDigits(pos, 10);
            return;
        }
        error("MalformedFpLit");
        scanner->spointer = sp1;
    }
}

void Tokenizer::scanFractionAndSuffix(int pos) {
    radix = 10;
    scanFraction(pos);
    if (scanner->c == 'f' || scanner->c == 'F') {
        scanner->putChar(true);
        tokenType = TokenType::FLOAT_LITERAL;
    } else {
        if (scanner->c == 'd' || scanner->c == 'D') {
            scanner->putChar(true);
        }
        tokenType = TokenType::DOUBLE_LITERAL;
    }
}

void Tokenizer::scanHexFractionAndSuffix(int pos, bool seendigit) {
    radix = 16;
    scanner->putChar(true);
    if (scanner->digit(pos, 16) >= 0) {
        seendigit = true;
        scanDigits(pos, 16);
    }
    if (!seendigit)
        error("Invalid Hex Number");
    else
        scanHexExponentAndSuffix(pos);
}

void Tokenizer::scanLitChar(int pos, bool translateEscapesNow, bool multiline) {
    if (scanner->c == u'\\') {
        if (scanner->peekChar() == u'\\' && !scanner->isUnicode()) {
            scanner->skipChar();
            if (!translateEscapesNow) {
                scanner->putChar(false);
            }
            scanner->putChar(true);
        } else {
            scanner->nextChar(translateEscapesNow);
            switch (scanner->c) {
                case '0': case '1': case '2': case '3':
                case '4': case '5': case '6': case '7': {
                    char16_t leadch = scanner->c;
                    int oct = scanner->digit(pos, 8);
                    scanner->nextChar(translateEscapesNow);
                    if ('0' <= scanner->c && scanner->c <= '7') {
                        oct = oct * 8 + scanner->digit(pos, 8);
                        scanner->nextChar(translateEscapesNow);
                        if (leadch <= '3' && '0' <= scanner->c && scanner->c <= '7') {
                            oct = oct * 8 + scanner->digit(pos, 8);
                            scanner->nextChar(translateEscapesNow);
                        }
                    }
                    if (translateEscapesNow) {
                        scanner->putChar((char16_t)oct);
                    }
                    break;
                }
                case 'b':
                    scanner->putChar(translateEscapesNow ? u'\b' : u'b', true);
                    break;
                case 't':
                    scanner->putChar(translateEscapesNow ? u'\t' : u't', true);
                    break;
                case 'n':
                    scanner->putChar(translateEscapesNow ? u'\n' : u'n', true);
                    break;
                case 'f':
                    scanner->putChar(translateEscapesNow ? u'\f' : u'f', true);
                    break;
                case 'r':
                    scanner->putChar(translateEscapesNow ? u'\r' : u'r', true);
                    break;
                case '\'':
                case '"':
                case '\\':
                    scanner->putChar(true);
                    break;
                case 's':
                    scanner->putChar(translateEscapesNow ? u' ' : u's', true);
                    break;
                case '\n':
                case '\r':
                    if (!multiline) {
                        error("Illegal Esc Char");
                    } else {
                        if (scanner->pointer == '\r' && scanner->peekChar() == '\n') {
                            scanner->nextChar(translateEscapesNow);
                        }
                        scanner->nextChar(translateEscapesNow);
                    }
                    break;
                default:
                    error("Illegal Esc Char");
            }
        }
    } else if (scanner->pointer != (int)scanner->buffer.size()) {
        scanner->putChar(true);
    }
}

int Tokenizer::countChar(char16_t ch, int max) {
    int count = 0;
    for (; count < max && scanner->pointer < (int)scanner->buffer.size() && scanner->c == ch; count++) {
        scanner->scanChar();
    }
    return count;
}

void Tokenizer::skipLineTerminator() {
    if (scanner->c == UnicodeScanner::CR && scanner->peekChar() == UnicodeScanner::LF) {
        scanner->scanChar();
    }
    scanner->scanChar();
}

void Tokenizer::scanString(int pos) {
    isTextBlock = false;
    hasEscapeSequences = false;
    int firstEOLN = -1;
    int openCount = countChar(u'"', 3);
    switch (openCount) {
        case 1:
            break;
        case 2:
            tokenType = TokenType::STRING_LITERAL;
            return;
        case 3:
            isTextBlock = true;
            while (scanner->pointer < (int)scanner->buffer.size()) {
                char16_t ch = scanner->c;
                if (ch != ' ' && ch != '\t' && ch != UnicodeScanner::FF) {
                    break;
                }
                scanner->scanChar();
            }
            if (scanner->c == UnicodeScanner::LF || scanner->c == UnicodeScanner::CR) {
                skipLineTerminator();
            } else {
                error("Illegal TextBlock Open");
                return;
            }
            break;
    }
    while (scanner->pointer < (int)scanner->buffer.size()) {
        if (scanner->c == u'"') {
            int closeCount = countChar(u'"', openCount);
            if (openCount == closeCount) {
                tokenType = TokenType::STRING_LITERAL;
                return;
            }
            scanner->repeat(u'"', closeCount);
        } else if (scanner->c == UnicodeScanner::LF || scanner->c == UnicodeScanner::CR) {
            if (openCount == 1) {
                break;
            }
            skipLineTerminator();
            scanner->putChar(u'\n', false);
            if (firstEOLN == -1) {
                firstEOLN = scanner->pointer;
            }
        } else if (scanner->c == u'\\') {
            hasEscapeSequences = true;
            scanLitChar(pos, true, openCount != 1);
        } else {
            scanner->putChar(true);
        }
    }
    error(openCount == 1 ? "Unclosed StrLit" : "Unclosed TextBlock");
    if (firstEOLN != -1) {
        scanner->reset(firstEOLN);
    }
}
