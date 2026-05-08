#pragma once
#include <string>
#include <vector>
#include "../tokenizer/Token.h"
#include "../ast/Expr.h"
#include "../ast/Stmt.h"
#include "../ast/Type.h"

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);
    ExprPtr parseExpression();
    StmtPtr parseStatement();
    std::unique_ptr<BlockStmt> parseBlock();
    TypePtr parseType();
    std::vector<StmtPtr> parseProgram();
    bool atEnd() const;

private:
    std::vector<Token> tokens;
    size_t pos = 0;

    const Token& peek() const;
    const Token& consume();
    bool check(TokenType type) const;
    bool match(TokenType type);
    const Token& expect(TokenType type, const char* what);
    bool isPrimitiveType(TokenType t) const;
    bool looksLikeTypedDecl() const;

    ExprPtr parsePrecedence(int minPrec);
    ExprPtr parsePrefix();
    int infixPrecedence(TokenType type) const;
    bool isRightAssoc(TokenType type) const;

    StmtPtr parseLet();
    StmtPtr parseTypedVarDecl();
    StmtPtr parseIf();
    StmtPtr parseWhile();
    StmtPtr parseReturn();
    StmtPtr parseExprStmt();

    [[noreturn]] void error(const Token& t, const std::string& msg) const;
};
