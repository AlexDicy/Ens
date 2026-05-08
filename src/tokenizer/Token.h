#pragma once
#include <string>
#include "TokenType.h"

class Token {
public:
    Token(TokenType type, std::u16string text, int line, int column)
        : type(type), text(std::move(text)), line(line), column(column) {}

    TokenType getType() const { return type; }
    const std::u16string& getText() const { return text; }
    int getLine() const { return line; }
    int getColumn() const { return column; }
    int getLength() const { return static_cast<int>(text.size()); }

private:
    TokenType type;
    std::u16string text;
    int line;
    int column;
};
