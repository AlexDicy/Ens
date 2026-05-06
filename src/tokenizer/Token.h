#pragma once
#include <string>
#include "TokenType.h"

class Token {
public:
    Token(TokenType type, std::u16string text, int line, int startColumn)
        : type(type), text(std::move(text)), line(line),
          startColumn(startColumn), endColumn(startColumn + (int)this->text.size()) {}

    TokenType getType() const { return type; }
    const std::u16string& getText() const { return text; }

private:
    TokenType type;
    std::u16string text;
    int line;
    int startColumn;
    int endColumn;
};
