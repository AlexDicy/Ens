#include "Statement.h"

namespace ast {

bool Statement::isStatementKind(SyntaxKind k) {
    switch (k) {
        case SyntaxKind::Block:
        case SyntaxKind::LetStmt:
        case SyntaxKind::TypedVarDecl:
        case SyntaxKind::IfStmt:
        case SyntaxKind::WhileStmt:
        case SyntaxKind::ReturnStmt:
        case SyntaxKind::ExprStmt:
        case SyntaxKind::ThrowStmt:
        case SyntaxKind::RethrowStmt:
            return true;
        default:
            return false;
    }
}

std::optional<Statement> Statement::cast(const SyntaxNode& n) {
    if (!isStatementKind(n.kind())) return std::nullopt;
    return Statement{n};
}

std::optional<Block>                 Statement::asBlock()           const { return Block::cast(node); }
std::optional<LetStatement>          Statement::asLet()             const { return LetStatement::cast(node); }
std::optional<TypedVarDeclStatement> Statement::asTypedVarDecl()    const { return TypedVarDeclStatement::cast(node); }
std::optional<IfStatement>           Statement::asIf()              const { return IfStatement::cast(node); }
std::optional<WhileStatement>        Statement::asWhile()           const { return WhileStatement::cast(node); }
std::optional<ReturnStatement>       Statement::asReturn()          const { return ReturnStatement::cast(node); }
std::optional<ExpressionStatement>   Statement::asExpressionStmt()  const { return ExpressionStatement::cast(node); }
std::optional<ThrowStatement>        Statement::asThrow()           const { return ThrowStatement::cast(node); }
std::optional<RethrowStatement>      Statement::asRethrow()         const { return RethrowStatement::cast(node); }

// === Block ===

std::vector<Statement> Block::statements() const {
    std::vector<Statement> out;
    for (auto& c : node.children()) {
        if (auto s = Statement::cast(c)) out.push_back(*s);
    }
    return out;
}

// === LetStatement ===

std::optional<SyntaxNode> LetStatement::nameToken() const {
    bool seenLet = false;
    for (auto& c : node.children()) {
        if (isTrivia(c.kind())) continue;
        if (c.kind() == SyntaxKind::KwLet) { seenLet = true; continue; }
        if (seenLet && c.kind() == SyntaxKind::Identifier) return c;
    }
    return std::nullopt;
}

std::optional<std::u16string> LetStatement::nameText() const {
    if (auto t = nameToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}

std::optional<Expression> LetStatement::initializer() const {
    for (auto& c : node.children()) {
        if (auto e = Expression::cast(c)) return e;
    }
    return std::nullopt;
}

// === TypedVarDeclStatement ===

std::optional<TypeReference> TypedVarDeclStatement::typeReference() const {
    if (auto tr = firstChildNode(node, SyntaxKind::TypeRef)) {
        return TypeReference::cast(*tr);
    }
    return std::nullopt;
}

std::optional<SyntaxNode> TypedVarDeclStatement::nameToken() const {
    bool seenType = false;
    for (auto& c : node.children()) {
        if (isTrivia(c.kind())) continue;
        if (c.kind() == SyntaxKind::TypeRef) { seenType = true; continue; }
        if (seenType && c.kind() == SyntaxKind::Identifier) return c;
    }
    return std::nullopt;
}

std::optional<std::u16string> TypedVarDeclStatement::nameText() const {
    if (auto t = nameToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}

std::optional<Expression> TypedVarDeclStatement::initializer() const {
    for (auto& c : node.children()) {
        if (auto e = Expression::cast(c)) return e;
    }
    return std::nullopt;
}

// === IfStatement ===

std::optional<Expression> IfStatement::condition() const {
    for (auto& c : node.children()) {
        if (auto e = Expression::cast(c)) return e;
    }
    return std::nullopt;
}

std::optional<Block> IfStatement::thenBlock() const {
    if (auto b = firstChildNode(node, SyntaxKind::Block)) return Block::cast(*b);
    return std::nullopt;
}

std::optional<ElseClause> IfStatement::elseClause() const {
    if (auto e = firstChildNode(node, SyntaxKind::ElseClause)) return ElseClause::cast(*e);
    return std::nullopt;
}

// === ElseClause ===

std::optional<Block> ElseClause::block() const {
    if (auto b = firstChildNode(node, SyntaxKind::Block)) return Block::cast(*b);
    return std::nullopt;
}

std::optional<IfStatement> ElseClause::ifStatement() const {
    if (auto i = firstChildNode(node, SyntaxKind::IfStmt)) return IfStatement::cast(*i);
    return std::nullopt;
}

// === WhileStatement ===

std::optional<Expression> WhileStatement::condition() const {
    for (auto& c : node.children()) {
        if (auto e = Expression::cast(c)) return e;
    }
    return std::nullopt;
}

std::optional<Block> WhileStatement::body() const {
    if (auto b = firstChildNode(node, SyntaxKind::Block)) return Block::cast(*b);
    return std::nullopt;
}

// === ReturnStatement ===

std::optional<Expression> ReturnStatement::value() const {
    for (auto& c : node.children()) {
        if (auto e = Expression::cast(c)) return e;
    }
    return std::nullopt;
}

// === ExpressionStatement ===

std::optional<Expression> ExpressionStatement::expression() const {
    for (auto& c : node.children()) {
        if (auto e = Expression::cast(c)) return e;
    }
    return std::nullopt;
}

// === ThrowStatement ===

std::optional<Expression> ThrowStatement::value() const {
    for (auto& c : node.children()) {
        if (auto e = Expression::cast(c)) return e;
    }
    return std::nullopt;
}

}  // namespace ast
