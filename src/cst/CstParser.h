#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>
#include "CstTokenizer.h"
#include "Green.h"
#include "SyntaxBuilder.h"
#include "SyntaxKind.h"

class DiagnosticSink;

class CstParser {
public:
    CstParser(std::u16string_view source, DiagnosticSink& sink);

    GreenElementPtr parseSourceFile();

private:
    std::u16string_view source;
    DiagnosticSink& sink;
    std::vector<LexedToken> tokens;
    size_t nextToEmit = 0;
    size_t current = 0;
    SyntaxBuilder builder;

    // === Cursor / token API ===
    SyntaxKind kindAt() const;
    const LexedToken& tokenAt() const;
    bool at(SyntaxKind k) const;
    bool atAny(std::initializer_list<SyntaxKind> kinds) const;
    bool atEnd() const;

    // Peek the n-th non-trivia token ahead (n=0 == current).
    SyntaxKind peekKind(size_t n) const;

    // Emit current token (and any preceding trivia) into the builder, advance.
    void bump();
    bool eat(SyntaxKind k);

    void expect(SyntaxKind k, const char* what);
    void emitMissing(SyntaxKind expectedKind, const char* what);

    void reportAtCurrent(std::string message);
    void recoverTo(std::initializer_list<SyntaxKind> syncSet);

    // === Decl-level ===
    void parseTopLevel();
    void parseVisibilityModifier();
    bool looksLikeFuncDecl(bool allowShorthand) const;
    bool looksLikeTypedVarDecl() const;
    void parseFuncDecl();
    void parseParamList();
    void parseParameter();
    void parseDefaultValue();
    void parseReturnType();
    void parseStructOrClassDecl(SyntaxKind nodeKind, SyntaxKind keywordKind);
    void parseStructOrClassMember();
    void parseFieldDecl();

    // === Type ===
    void parseType();
    bool isTypeStart(SyntaxKind k) const;
    bool isPrimitiveTypeKw(SyntaxKind k) const;

    // === Statements ===
    void parseStatement();
    void parseBlock();
    void parseLetStmt();
    void parseTypedVarDeclStmt();
    void parseIfStmt();
    void parseWhileStmt();
    void parseReturnStmt();
    void parseExprStmt();

    // === Expressions ===
    void parseExpression();
    void parsePrecedence(int minPrec);
    void parsePrefix();
    void parseArgList();

    int infixPrecedence(SyntaxKind k) const;
    bool isAssignmentOp(SyntaxKind k) const;
};
