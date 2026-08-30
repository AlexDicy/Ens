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
    // True for the duration of a switch arm's label list, where a '(' never opens a lambda
    // and an identifier before '->' is the label rather than a bare parameter: `(one) -> body`
    // and `Circle -> body` are the arm.
    bool inArmLabels = false;

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
    // True when positioned on the contextual `primitive` keyword (a `primitive` identifier).
    bool atContextualPrimitive() const;
    bool peekIsContextualPrimitive(size_t n) const;
    // True when a primitive binding starts `ahead` significant tokens from the cursor.
    bool atPrimitiveBinding(size_t ahead) const;

    void expect(SyntaxKind k, const char* what);
    void emitMissing(SyntaxKind expectedKind, const char* what);

    // The current token's text for a diagnostic; non-ASCII code units become '?'.
    std::string asciiTokenText() const;
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
    void parsePrimitiveDecl();
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
    void parseNamedType(bool leaveCoalesceToExpression);
    void parseFunctionType();
    void parseParenthesizedType(bool leaveCoalesceToExpression);
    // The `?` / `[]` chain, emitted into the type node the caller has open.
    void parseTypeSuffixes(bool leaveCoalesceToExpression);
    void parseTypeHead();  // base + namespace only, no [] or ? (used in `new`)
    bool isTypeStart(SyntaxKind k) const;
    // Where a whole type is expected rather than a named one: a '(' opens a function type
    // or a type in parentheses.
    bool isTypeOrGroupStart(SyntaxKind k) const;
    bool isPrimitiveTypeKw(SyntaxKind k) const;
    void parseTypeArgList();
    void expectClosingGt(const char* what);
    bool atClosingGt() const;
    size_t scanTypeArgs(size_t cursor) const;  // peek-index past a type-ish <...>, else 0
    // The peek-index just past a whole type written at `cursor`, or 0 when none is. The
    // token-level counterpart of parseType, for the lookaheads that classify a statement.
    size_t scanType(size_t cursor, int depth = 0) const;
    static constexpr int kMaxTypeScanDepth = 32;
    size_t skipAnglesRaw(size_t idx) const;    // raw-index past <...>, else idx
    // At '(': true when the token after the matching ')' is '->'. Both readings of a leading
    // '(' ask this one question: in type position it tells a function type from a type in
    // parentheses, and in expression position a lambda from a parenthesized expression.
    bool atArrowAfterParentheses() const;

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
    void parseLambda();
    void parseBareParameterLambda();
    void parseLambdaParameters();
    void parseLambdaParameter();
    void parseLambdaBody();

    int infixPrecedence(SyntaxKind k) const;
    bool isAssignmentOp(SyntaxKind k) const;
    // A '{' whose next two tokens are `IDENT :` starts a struct literal; every
    // other '{' is a block.
    bool atStructLiteralStart() const;
};
