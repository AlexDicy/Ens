#pragma once
#include <stdexcept>
#include <string>
#include <vector>
#include "../tokenizer/Token.h"
#include "../ast/Expr.h"

class ParseError : public std::runtime_error {
public:
    int line;
    ParseError(int line, const std::string& msg)
        : std::runtime_error(msg), line(line) {}
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);
    ExprPtr parseExpression();
    bool atEnd() const;

private:
    std::vector<Token> tokens;
    size_t pos = 0;

    const Token& peek() const;
    const Token& consume();
    bool check(TokenType type) const;
    bool match(TokenType type);
    const Token& expect(TokenType type, const char* what);

    ExprPtr parsePrecedence(int minPrec);
    ExprPtr parsePrefix();
    int infixPrecedence(TokenType type) const;
    bool isRightAssoc(TokenType type) const;

    [[noreturn]] void error(const Token& t, const std::string& msg) const;
};
