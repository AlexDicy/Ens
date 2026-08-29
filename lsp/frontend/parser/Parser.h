#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>
#include "Tokenizer.h"
#include "../cst/Green.h"
#include "../cst/SyntaxBuilder.h"
#include "../cst/SyntaxKind.h"

class DiagnosticSink;

class Parser {
public:
    Parser(std::u16string_view source, DiagnosticSink& sink);

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

    // At the first `?` of a `??` written with the two adjacent.
    bool atNullCoalesce() const;

    // Emit current token (and any preceding trivia) into the builder, advance.
    void bump();
    // Like bump(), but record the current token under an overridden kind (for
    // contextual keywords).
    void bumpAs(SyntaxKind kind);
    bool eat(SyntaxKind k);
    // True when positioned on the contextual `out` keyword (an `out` identifier).
    bool atContextualOut() const;
    // True when positioned on the contextual `test` keyword (a `test` identifier).
    bool atContextualTest() const;
    bool peekIsContextualTest(size_t n) const;
    // True when positioned on the contextual `from` keyword (a `from` identifier).
    bool atContextualFrom() const;
    bool peekIsContextualFrom(size_t n) const;
    // True when positioned on the contextual `type` keyword (a `type` identifier).
    bool atContextualType() const;
    bool peekIsContextualType(size_t n) const;

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
    void parseTestDecl();
    void parseParamList();
    void parseParameter();
    void parseDefaultValue();
    void parseReturnType();
    void parseThrowsClause();
    void parseCatchClause();
    void parseStructOrClassDecl(SyntaxKind nodeKind, SyntaxKind keywordKind);
    void parseInterfaceDecl();
    void parseImplementsClause();
    void parseStructOrClassMember();
    bool looksLikeKeywordNamedMethod() const;
    void parseFieldDecl();
    void parseEnumDecl();
    void parseTypeParamList();
    void parseTypeParam();
    void parseImportDecl();
    void parseImportPath();
    void parseExternalDecl();
    void parseExternalBlock();
    void parseExternalTypeDecl();
    void parseExternalFuncDecl();

    // === Type ===
    // `leaveCoalesceToExpression` stops the suffix chain at a `?` that opens an adjacent `??`
    // pair, which only the target of `as`/`as?` wants: it is the one type position an
    // expression continues from.
    void parseType(bool leaveCoalesceToExpression = false);
    void parseTypeHead();  // base + namespace only, no [] or ? (used in `new`)
    bool isTypeStart(SyntaxKind k) const;
    bool isPrimitiveTypeKw(SyntaxKind k) const;
    void parseTypeArgList();
    void expectClosingGt(const char* what);
    bool atClosingGt() const;
    size_t scanTypeArgs(size_t cursor) const;  // peek-index past a type-ish <...>, else 0
    size_t skipAnglesRaw(size_t idx) const;    // raw-index past <...>, else idx

    // === Statements ===
    void parseStatement();
    void parseBlock();
    void parseLetStmt();
    void parseTypedVarDeclStmt();
    void parseConstDecl();
    void parseIfStmt();
    void parseWhileStmt();
    void parseForStmt();
    void parseBreakStmt();
    void parseContinueStmt();
    void parseReturnStmt();
    void parseThrowStmt();
    void parseRethrowStmt();
    void parseExprStmt();
    void parseSwitchStmt();
    void parseSwitchHeaderAndArms();
    void parseSwitchArm();

    bool looksLikeForeachHeader() const;
    bool looksLikeTypedVarDeclFrom(size_t startCursor) const;
    bool atContextualIn() const;
    bool peekIsContextualIn(size_t n) const;

    // === Expressions ===
    void parseExpression();
    void parsePrecedence(int minPrec);
    void parsePrefix();
    void parseSwitchExpr();
    void parseArgList();
    void parseCallArgument();
    void parseStructLiteral();
    void parseStructLiteralField();

    int infixPrecedence(SyntaxKind k) const;
    bool isAssignmentOp(SyntaxKind k) const;
    // A '{' whose next two tokens are `IDENT :` starts a struct literal; every
    // other '{' is a block.
    bool atStructLiteralStart() const;
};
