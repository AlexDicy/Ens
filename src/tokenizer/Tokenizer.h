#pragma once
#include <optional>
#include <vector>
#include "Token.h"

class UnicodeScanner;

class Tokenizer {
public:
    static std::vector<Token> tokenize(std::u16string_view code);
    std::optional<Token> readToken();

protected:
    int radix = 0;
    bool isTextBlock = false;
    bool hasEscapeSequences = false;

private:
    UnicodeScanner* scanner = nullptr;
    std::optional<TokenType> tokenType;

    void error(int pos, const char* msg);
    bool isSpecial(char16_t ch);
    void scanOperator();
    void scanIdentifier();
    void scanNumber(int pos, int radix);
    void scanDigits(int pos, int digitRadix);
    void scanHexExponentAndSuffix(int pos);
    void scanFraction(int pos);
    void scanFractionAndSuffix(int pos);
    void scanHexFractionAndSuffix(int pos, bool seendigit);
    void scanLitChar(int pos, bool translateEscapesNow, bool multiline);
    int countChar(char16_t ch, int max);
    void skipLineTerminator();
    void scanString(int pos);
};
