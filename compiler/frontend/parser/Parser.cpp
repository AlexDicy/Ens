#include "Parser.h"

#include <algorithm>
#include "../diagnostics/Diagnostic.h"
#include "../diagnostics/DiagnosticSink.h"

Parser::Parser(std::u16string_view src, DiagnosticSink& s)
    : source(src), sink(s) {
    Tokenizer tokenizer(source, sink);
    while (true) {
        LexedToken t = tokenizer.next();
        bool eof = (t.kind == SyntaxKind::EndOfFile);
        tokens.push_back(std::move(t));
        if (eof) break;
    }
    // Position `current` at the first non-trivia token.
    while (current < tokens.size() && isTrivia(tokens[current].kind)) current++;
}

SyntaxKind Parser::kindAt() const {
    return current < tokens.size() ? tokens[current].kind : SyntaxKind::EndOfFile;
}

const LexedToken& Parser::tokenAt() const {
    return tokens[current];
}

bool Parser::at(SyntaxKind k) const {
    return kindAt() == k;
}

bool Parser::atAny(std::initializer_list<SyntaxKind> kinds) const {
    SyntaxKind k = kindAt();
    for (auto x : kinds) if (x == k) return true;
    return false;
}

bool Parser::atEnd() const {
    return kindAt() == SyntaxKind::EndOfFile;
}

SyntaxKind Parser::peekKind(size_t n) const {
    size_t idx = current;
    while (true) {
        if (idx >= tokens.size()) return SyntaxKind::EndOfFile;
        if (!isTrivia(tokens[idx].kind)) {
            if (n == 0) return tokens[idx].kind;
            n--;
        }
        idx++;
    }
}

void Parser::bump() {
    while (nextToEmit < tokens.size() && nextToEmit < current) {
        const auto& t = tokens[nextToEmit];
        builder.token(t.kind, t.text);
        nextToEmit++;
    }
    if (current < tokens.size() && !atEnd()) {
        const auto& t = tokens[current];
        builder.token(t.kind, t.text);
        nextToEmit = current + 1;
        current++;
        while (current < tokens.size() && isTrivia(tokens[current].kind)) current++;
    }
}

bool Parser::eat(SyntaxKind k) {
    if (!at(k)) return false;
    bump();
    return true;
}

void Parser::expect(SyntaxKind k, const char* what) {
    if (at(k)) {
        bump();
    } else {
        emitMissing(k, what);
    }
}

void Parser::emitMissing(SyntaxKind /*expectedKind*/, const char* what) {
    // Flush trivia preceding the current cursor position so the missing-token
    // marker lands at the right offset in the tree.
    while (nextToEmit < tokens.size() && nextToEmit < current) {
        const auto& t = tokens[nextToEmit];
        builder.token(t.kind, t.text);
        nextToEmit++;
    }
    builder.token(SyntaxKind::Missing, std::u16string{});
    reportAtCurrent(std::string("Expected ") + what);
}

void Parser::reportAtCurrent(std::string message) {
    int line, column, length;
    if (current < tokens.size()) {
        line = tokens[current].line;
        column = tokens[current].column;
        length = std::max<int>(1, static_cast<int>(tokens[current].text.size()));
    } else {
        line = 1;
        column = 1;
        length = 1;
    }
    sink.error({line, column, length}, std::move(message));
}

void Parser::recoverTo(std::initializer_list<SyntaxKind> syncSet) {
    if (atEnd()) return;
    builder.startNode(SyntaxKind::Error);
    bool any = false;
    while (!atEnd()) {
        SyntaxKind k = kindAt();
        bool inSet = false;
        for (auto x : syncSet) if (x == k) { inSet = true; break; }
        if (inSet) break;
        bump();
        any = true;
    }
    if (!any) {
        // Avoid an empty Error node; finish it anyway since startNode was called.
    }
    builder.finishNode();
}

// =================================================================
// Top level
// =================================================================

GreenElementPtr Parser::parseSourceFile() {
    builder.startNode(SyntaxKind::SourceFile);
    while (!atEnd()) {
        size_t before = current;
        parseTopLevel();
        if (current == before) {
            // Defensive: if no progress was made, force advance to avoid infinite loop.
            reportAtCurrent("Unexpected token at top level");
            recoverTo({SyntaxKind::KwImport, SyntaxKind::KwStruct, SyntaxKind::KwClass,
                       SyntaxKind::KwPrivate, SyntaxKind::KwProtected, SyntaxKind::KwPublic,
                       SyntaxKind::Identifier, SyntaxKind::Semi, SyntaxKind::EndOfFile});
            if (current == before && !atEnd()) bump();
        }
    }
    // Flush any trailing trivia at end of file.
    while (nextToEmit < tokens.size() && tokens[nextToEmit].kind != SyntaxKind::EndOfFile) {
        const auto& t = tokens[nextToEmit];
        builder.token(t.kind, t.text);
        nextToEmit++;
    }
    builder.finishNode();
    return builder.build();
}

void Parser::parseTopLevel() {
    if (at(SyntaxKind::KwImport)) {
        parseImportDecl();
        return;
    }

    bool hasVisibility = atAny({SyntaxKind::KwPrivate, SyntaxKind::KwProtected, SyntaxKind::KwPublic});

    if (at(SyntaxKind::KwStruct) ||
        peekKind(hasVisibility ? 1 : 0) == SyntaxKind::KwStruct) {
        parseStructOrClassDecl(SyntaxKind::StructDecl, SyntaxKind::KwStruct);
        return;
    }
    if (at(SyntaxKind::KwClass) ||
        peekKind(hasVisibility ? 1 : 0) == SyntaxKind::KwClass) {
        parseStructOrClassDecl(SyntaxKind::ClassDecl, SyntaxKind::KwClass);
        return;
    }

    // Check function decl first, then typed var decl.
    if (looksLikeFuncDecl(/*allowShorthand=*/false)) {
        parseFuncDecl();
        return;
    }
    if (looksLikeTypedVarDecl()) {
        parseTypedVarDeclStmt();
        return;
    }

    // Fallback: treat as statement to recover from misplaced code.
    if (atEnd()) return;
    parseStatement();
}

void Parser::parseVisibilityModifier() {
    if (!atAny({SyntaxKind::KwPrivate, SyntaxKind::KwProtected, SyntaxKind::KwPublic})) return;
    builder.startNode(SyntaxKind::VisibilityModifier);
    bump();
    builder.finishNode();
}

// =================================================================
// Import declarations
// =================================================================

void Parser::parseImportDecl() {
    builder.startNode(SyntaxKind::ImportDecl);
    expect(SyntaxKind::KwImport, "'import'");

    // Optional alias: `import Identifier from path;`
    if (peekKind(0) == SyntaxKind::Identifier && peekKind(1) == SyntaxKind::KwFrom) {
        bump();  // alias identifier
        bump();  // 'from'
    }

    parseImportPath();
    expect(SyntaxKind::Semi, "';' after import");
    builder.finishNode();
}

void Parser::parseImportPath() {
    builder.startNode(SyntaxKind::ImportPath);
    eat(SyntaxKind::At);  // optional '@' prefix for package imports
    expect(SyntaxKind::Identifier, "path segment");
    while (at(SyntaxKind::Dot) && peekKind(1) == SyntaxKind::Identifier) {
        bump();  // '.'
        bump();  // identifier
    }
    builder.finishNode();
}

// =================================================================
// Function declarations
// =================================================================

bool Parser::looksLikeFuncDecl(bool allowShorthand) const {
    size_t idx = current;
    // Skip an optional visibility modifier.
    if (idx < tokens.size() && (tokens[idx].kind == SyntaxKind::KwPrivate ||
                                tokens[idx].kind == SyntaxKind::KwProtected ||
                                tokens[idx].kind == SyntaxKind::KwPublic)) {
        idx++;
        while (idx < tokens.size() && isTrivia(tokens[idx].kind)) idx++;
    }
    if (idx >= tokens.size() || tokens[idx].kind != SyntaxKind::Identifier) return false;
    idx++;
    while (idx < tokens.size() && isTrivia(tokens[idx].kind)) idx++;
    if (idx >= tokens.size() || tokens[idx].kind != SyntaxKind::LParen) return false;

    // Walk balanced parens.
    int depth = 1;
    idx++;
    while (idx < tokens.size() && depth > 0) {
        SyntaxKind k = tokens[idx].kind;
        if (k == SyntaxKind::LParen) depth++;
        else if (k == SyntaxKind::RParen) depth--;
        else if (k == SyntaxKind::EndOfFile) return false;
        idx++;
    }
    if (depth != 0) return false;
    while (idx < tokens.size() && isTrivia(tokens[idx].kind)) idx++;
    if (idx >= tokens.size()) return false;
    SyntaxKind after = tokens[idx].kind;
    if (after == SyntaxKind::Arrow || after == SyntaxKind::LBrace) return true;
    return allowShorthand && after == SyntaxKind::Semi;
}

void Parser::parseFuncDecl() {
    builder.startNode(SyntaxKind::FuncDecl);
    parseVisibilityModifier();
    expect(SyntaxKind::Identifier, "function name");
    expect(SyntaxKind::LParen, "'(' after function name");
    parseParamList();
    expect(SyntaxKind::RParen, "')' after parameters");
    if (at(SyntaxKind::Arrow)) parseReturnType();
    if (at(SyntaxKind::LBrace)) {
        parseBlock();
    } else if (eat(SyntaxKind::Semi)) {
        // shorthand body
    } else {
        emitMissing(SyntaxKind::LBrace, "'{' or ';' for function body");
        recoverTo({SyntaxKind::RBrace, SyntaxKind::Semi,
                   SyntaxKind::KwStruct, SyntaxKind::KwClass,
                   SyntaxKind::KwPrivate, SyntaxKind::KwProtected, SyntaxKind::KwPublic,
                   SyntaxKind::EndOfFile});
        eat(SyntaxKind::Semi);
    }
    builder.finishNode();
}

void Parser::parseParamList() {
    builder.startNode(SyntaxKind::ParamList);
    if (!at(SyntaxKind::RParen) && !atEnd()) {
        parseParameter();
        while (eat(SyntaxKind::Comma)) {
            parseParameter();
        }
    }
    builder.finishNode();
}

void Parser::parseParameter() {
    builder.startNode(SyntaxKind::Parameter);
    if (eat(SyntaxKind::KwThis)) {
        expect(SyntaxKind::Dot, "'.' after 'this' in parameter");
        expect(SyntaxKind::Identifier, "field name after 'this.'");
    } else {
        if (isTypeStart(kindAt())) {
            parseType();
        } else {
            emitMissing(SyntaxKind::Identifier, "parameter type");
        }
        expect(SyntaxKind::Identifier, "parameter name");
    }
    if (at(SyntaxKind::Eq)) parseDefaultValue();
    builder.finishNode();
}

void Parser::parseDefaultValue() {
    builder.startNode(SyntaxKind::DefaultValue);
    bump();  // =
    parseExpression();
    builder.finishNode();
}

void Parser::parseReturnType() {
    builder.startNode(SyntaxKind::ReturnType);
    expect(SyntaxKind::Arrow, "'->'");
    if (isTypeStart(kindAt())) parseType();
    else emitMissing(SyntaxKind::Identifier, "return type");
    builder.finishNode();
}

// =================================================================
// Struct / class declarations
// =================================================================

void Parser::parseStructOrClassDecl(SyntaxKind nodeKind, SyntaxKind keywordKind) {
    builder.startNode(nodeKind);
    parseVisibilityModifier();
    expect(keywordKind, keywordKind == SyntaxKind::KwStruct ? "'struct'" : "'class'");
    expect(SyntaxKind::Identifier, "name");
    expect(SyntaxKind::LBrace, "'{'");
    builder.startNode(SyntaxKind::MemberList);
    while (!at(SyntaxKind::RBrace) && !atEnd()) {
        size_t before = current;
        parseStructOrClassMember();
        if (current == before) {
            reportAtCurrent("Unexpected token in member list");
            recoverTo({SyntaxKind::RBrace, SyntaxKind::Semi, SyntaxKind::EndOfFile});
            eat(SyntaxKind::Semi);
            if (current == before && !atEnd()) bump();
        }
    }
    builder.finishNode();
    expect(SyntaxKind::RBrace, "'}'");
    builder.finishNode();
}

void Parser::parseStructOrClassMember() {
    if (looksLikeFuncDecl(/*allowShorthand=*/true)) {
        parseFuncDecl();
    } else {
        parseFieldDecl();
    }
}

void Parser::parseFieldDecl() {
    builder.startNode(SyntaxKind::FieldDecl);
    parseVisibilityModifier();
    if (at(SyntaxKind::KwWeak)) bump();
    if (isTypeStart(kindAt())) parseType();
    else emitMissing(SyntaxKind::Identifier, "field type");
    expect(SyntaxKind::Identifier, "field name");
    if (at(SyntaxKind::Eq)) parseDefaultValue();
    expect(SyntaxKind::Semi, "';' after field declaration");
    builder.finishNode();
}

// =================================================================
// Types
// =================================================================

bool Parser::isPrimitiveTypeKw(SyntaxKind k) const {
    switch (k) {
        case SyntaxKind::KwBool:
        case SyntaxKind::KwByte:
        case SyntaxKind::KwShort:
        case SyntaxKind::KwUShort:
        case SyntaxKind::KwInt:
        case SyntaxKind::KwUInt:
        case SyntaxKind::KwLong:
        case SyntaxKind::KwULong:
        case SyntaxKind::KwFloat:
        case SyntaxKind::KwDouble:
        case SyntaxKind::KwDecimal:
        case SyntaxKind::KwChar:
        case SyntaxKind::KwString:
        case SyntaxKind::KwVoid:
            return true;
        default:
            return false;
    }
}

bool Parser::isTypeStart(SyntaxKind k) const {
    return k == SyntaxKind::Identifier || isPrimitiveTypeKw(k);
}

void Parser::parseType() {
    builder.startNode(SyntaxKind::TypeRef);
    bool wasIdentifier = at(SyntaxKind::Identifier);
    if (isTypeStart(kindAt())) bump();
    else emitMissing(SyntaxKind::Identifier, "type name");
    // Allow a single namespace qualifier: `ns.Name`. Primitives are not qualifiable.
    if (wasIdentifier && at(SyntaxKind::Dot) && peekKind(1) == SyntaxKind::Identifier) {
        bump();  // '.'
        bump();  // identifier
    }
    eat(SyntaxKind::Question);
    builder.finishNode();
}

bool Parser::looksLikeTypedVarDecl() const {
    SyntaxKind k0 = peekKind(0);
    if (!isTypeStart(k0)) return false;
    // Skip a single optional namespace qualifier: `ns.Name ...`. Primitives can't
    // be qualified, so this only fires when the leading token is an Identifier.
    size_t typeEnd = 1;
    if (k0 == SyntaxKind::Identifier &&
        peekKind(1) == SyntaxKind::Dot &&
        peekKind(2) == SyntaxKind::Identifier) {
        typeEnd = 3;
    }
    SyntaxKind nameOrAfterQ = peekKind(typeEnd);
    SyntaxKind afterName = peekKind(typeEnd + 1);
    if (nameOrAfterQ == SyntaxKind::Question) {
        nameOrAfterQ = peekKind(typeEnd + 1);
        afterName = peekKind(typeEnd + 2);
    }
    if (nameOrAfterQ != SyntaxKind::Identifier) return false;
    return afterName == SyntaxKind::Eq || afterName == SyntaxKind::Semi;
}

// =================================================================
// Statements
// =================================================================

void Parser::parseStatement() {
    SyntaxKind k = kindAt();
    if (k == SyntaxKind::LBrace)        { parseBlock(); return; }
    if (k == SyntaxKind::KwLet)         { parseLetStmt(); return; }
    if (k == SyntaxKind::KwIf)          { parseIfStmt(); return; }
    if (k == SyntaxKind::KwWhile)       { parseWhileStmt(); return; }
    if (k == SyntaxKind::KwReturn)      { parseReturnStmt(); return; }
    if (looksLikeTypedVarDecl())        { parseTypedVarDeclStmt(); return; }
    parseExprStmt();
}

void Parser::parseBlock() {
    builder.startNode(SyntaxKind::Block);
    expect(SyntaxKind::LBrace, "'{'");
    while (!at(SyntaxKind::RBrace) && !atEnd()) {
        size_t before = current;
        parseStatement();
        if (current == before) {
            reportAtCurrent("Unexpected token in block");
            recoverTo({SyntaxKind::Semi, SyntaxKind::RBrace, SyntaxKind::EndOfFile});
            eat(SyntaxKind::Semi);
            if (current == before && !atEnd()) bump();
        }
    }
    expect(SyntaxKind::RBrace, "'}'");
    builder.finishNode();
}

void Parser::parseLetStmt() {
    builder.startNode(SyntaxKind::LetStmt);
    expect(SyntaxKind::KwLet, "'let'");
    expect(SyntaxKind::Identifier, "identifier after 'let'");
    if (eat(SyntaxKind::Colon)) {
        if (isTypeStart(kindAt())) parseType();
        else emitMissing(SyntaxKind::Identifier, "type after ':'");
    }
    if (eat(SyntaxKind::Eq)) parseExpression();
    expect(SyntaxKind::Semi, "';' after let declaration");
    builder.finishNode();
}

void Parser::parseTypedVarDeclStmt() {
    builder.startNode(SyntaxKind::TypedVarDecl);
    parseType();
    expect(SyntaxKind::Identifier, "identifier after type");
    if (eat(SyntaxKind::Eq)) parseExpression();
    expect(SyntaxKind::Semi, "';' after declaration");
    builder.finishNode();
}

void Parser::parseIfStmt() {
    builder.startNode(SyntaxKind::IfStmt);
    expect(SyntaxKind::KwIf, "'if'");
    expect(SyntaxKind::LParen, "'(' around if condition");
    parseExpression();
    expect(SyntaxKind::RParen, "')' after if condition");
    if (at(SyntaxKind::LBrace)) parseBlock();
    else { emitMissing(SyntaxKind::LBrace, "'{' for if body"); }
    if (at(SyntaxKind::KwElse)) {
        builder.startNode(SyntaxKind::ElseClause);
        bump();
        if (at(SyntaxKind::KwIf)) parseIfStmt();
        else if (at(SyntaxKind::LBrace)) parseBlock();
        else emitMissing(SyntaxKind::LBrace, "'{' or 'if' after 'else'");
        builder.finishNode();
    }
    builder.finishNode();
}

void Parser::parseWhileStmt() {
    builder.startNode(SyntaxKind::WhileStmt);
    expect(SyntaxKind::KwWhile, "'while'");
    expect(SyntaxKind::LParen, "'(' around while condition");
    parseExpression();
    expect(SyntaxKind::RParen, "')' after while condition");
    if (at(SyntaxKind::LBrace)) parseBlock();
    else emitMissing(SyntaxKind::LBrace, "'{' for while body");
    builder.finishNode();
}

void Parser::parseReturnStmt() {
    builder.startNode(SyntaxKind::ReturnStmt);
    expect(SyntaxKind::KwReturn, "'return'");
    if (!at(SyntaxKind::Semi) && !atEnd()) parseExpression();
    expect(SyntaxKind::Semi, "';' after return");
    builder.finishNode();
}

void Parser::parseExprStmt() {
    builder.startNode(SyntaxKind::ExprStmt);
    parseExpression();
    expect(SyntaxKind::Semi, "';' after expression");
    builder.finishNode();
}

// =================================================================
// Expressions (Pratt)
// =================================================================

bool Parser::isAssignmentOp(SyntaxKind k) const {
    switch (k) {
        case SyntaxKind::Eq:
        case SyntaxKind::PlusEq:
        case SyntaxKind::MinusEq:
        case SyntaxKind::StarEq:
        case SyntaxKind::SlashEq:
        case SyntaxKind::PercentEq:
        case SyntaxKind::AmpEq:
        case SyntaxKind::PipeEq:
        case SyntaxKind::CaretEq:
        case SyntaxKind::LtLtEq:
        case SyntaxKind::GtGtEq:
        case SyntaxKind::GtGtGtEq:
            return true;
        default:
            return false;
    }
}

int Parser::infixPrecedence(SyntaxKind k) const {
    if (isAssignmentOp(k)) return 1;
    switch (k) {
        case SyntaxKind::Question:   return 2;   // ternary
        case SyntaxKind::PipePipe:   return 3;
        case SyntaxKind::AmpAmp:     return 4;
        case SyntaxKind::Pipe:       return 5;
        case SyntaxKind::Caret:      return 6;
        case SyntaxKind::Amp:        return 7;
        case SyntaxKind::EqEq:
        case SyntaxKind::NotEq:      return 8;
        case SyntaxKind::Lt:
        case SyntaxKind::Gt:
        case SyntaxKind::LtEq:
        case SyntaxKind::GtEq:       return 9;
        case SyntaxKind::LtLt:
        case SyntaxKind::GtGt:
        case SyntaxKind::GtGtGt:     return 10;
        case SyntaxKind::Plus:
        case SyntaxKind::Minus:      return 11;
        case SyntaxKind::Star:
        case SyntaxKind::Slash:
        case SyntaxKind::Percent:    return 12;
        case SyntaxKind::Dot:
        case SyntaxKind::QuestionDot:
        case SyntaxKind::LParen:
        case SyntaxKind::LBracket:   return 14;  // postfix
        default:                     return 0;
    }
}

void Parser::parseExpression() {
    parsePrecedence(1);
}

void Parser::parsePrecedence(int minPrec) {
    size_t cp = builder.checkpoint();
    parsePrefix();

    while (true) {
        SyntaxKind op = kindAt();
        int prec = infixPrecedence(op);
        if (prec < minPrec) break;

        if (op == SyntaxKind::LParen) {
            builder.startNodeAt(cp, SyntaxKind::CallExpr);
            parseArgList();
            builder.finishNode();
            continue;
        }
        if (op == SyntaxKind::LBracket) {
            builder.startNodeAt(cp, SyntaxKind::SubscriptExpr);
            bump();
            parseExpression();
            expect(SyntaxKind::RBracket, "']' after subscript");
            builder.finishNode();
            continue;
        }
        if (op == SyntaxKind::Dot) {
            builder.startNodeAt(cp, SyntaxKind::MemberExpr);
            bump();
            expect(SyntaxKind::Identifier, "identifier after '.'");
            builder.finishNode();
            continue;
        }
        if (op == SyntaxKind::QuestionDot) {
            builder.startNodeAt(cp, SyntaxKind::SafeMemberExpr);
            bump();
            expect(SyntaxKind::Identifier, "identifier after '?.'");
            builder.finishNode();
            continue;
        }
        if (op == SyntaxKind::Question) {
            builder.startNodeAt(cp, SyntaxKind::TernaryExpr);
            bump();
            parseExpression();
            expect(SyntaxKind::Colon, "':' in ternary expression");
            parsePrecedence(1);
            builder.finishNode();
            continue;
        }
        if (isAssignmentOp(op)) {
            builder.startNodeAt(cp, SyntaxKind::AssignExpr);
            bump();
            parsePrecedence(prec);
            builder.finishNode();
            continue;
        }
        builder.startNodeAt(cp, SyntaxKind::BinaryExpr);
        bump();
        parsePrecedence(prec + 1);
        builder.finishNode();
    }
}

void Parser::parsePrefix() {
    SyntaxKind k = kindAt();
    switch (k) {
        case SyntaxKind::IntLiteral:
        case SyntaxKind::LongLiteral:
        case SyntaxKind::FloatLiteral:
        case SyntaxKind::DoubleLiteral:
        case SyntaxKind::StringLiteral:
        case SyntaxKind::CharLiteral:
        case SyntaxKind::KwTrue:
        case SyntaxKind::KwFalse:
        case SyntaxKind::KwNull:
            builder.startNode(SyntaxKind::LiteralExpr);
            bump();
            builder.finishNode();
            return;
        case SyntaxKind::Identifier:
            builder.startNode(SyntaxKind::IdentExpr);
            bump();
            builder.finishNode();
            return;
        case SyntaxKind::KwThis:
            builder.startNode(SyntaxKind::ThisExpr);
            bump();
            builder.finishNode();
            return;
        case SyntaxKind::KwNew: {
            builder.startNode(SyntaxKind::NewExpr);
            bump();
            if (at(SyntaxKind::Identifier)) parseType();
            else emitMissing(SyntaxKind::Identifier, "type name after 'new'");
            if (at(SyntaxKind::LParen)) parseArgList();
            else emitMissing(SyntaxKind::LParen, "'(' after class name in 'new'");
            builder.finishNode();
            return;
        }
        case SyntaxKind::LParen: {
            builder.startNode(SyntaxKind::ParenExpr);
            bump();
            parseExpression();
            expect(SyntaxKind::RParen, "')'");
            builder.finishNode();
            return;
        }
        case SyntaxKind::Minus:
        case SyntaxKind::Bang:
        case SyntaxKind::PlusPlus:
        case SyntaxKind::MinusMinus: {
            builder.startNode(SyntaxKind::PrefixExpr);
            bump();
            parsePrecedence(13);
            builder.finishNode();
            return;
        }
        default:
            emitMissing(SyntaxKind::Identifier, "expression");
            return;
    }
}

void Parser::parseArgList() {
    builder.startNode(SyntaxKind::ArgList);
    expect(SyntaxKind::LParen, "'('");
    if (!at(SyntaxKind::RParen) && !atEnd()) {
        parseExpression();
        while (eat(SyntaxKind::Comma)) {
            if (at(SyntaxKind::RParen)) break;
            parseExpression();
        }
    }
    expect(SyntaxKind::RParen, "')'");
    builder.finishNode();
}
