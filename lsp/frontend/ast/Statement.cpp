#include "Statement.h"

namespace ast {

bool Statement::isStatementKind(SyntaxKind k) {
    switch (k) {
        case SyntaxKind::Block:
        case SyntaxKind::LetStmt:
        case SyntaxKind::TypedVarDecl:
        case SyntaxKind::IfStmt:
        case SyntaxKind::WhileStmt:
        case SyntaxKind::ForStmt:
        case SyntaxKind::ForEachStmt:
        case SyntaxKind::BreakStmt:
        case SyntaxKind::ContinueStmt:
        case SyntaxKind::ReturnStmt:
        case SyntaxKind::ExprStmt:
        case SyntaxKind::ThrowStmt:
        case SyntaxKind::RethrowStmt:
        case SyntaxKind::SwitchStmt:
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
std::optional<ForStatement>          Statement::asFor()             const { return ForStatement::cast(node); }
std::optional<ForEachStatement>      Statement::asForEach()         const { return ForEachStatement::cast(node); }
std::optional<BreakStatement>        Statement::asBreak()           const { return BreakStatement::cast(node); }
std::optional<ContinueStatement>     Statement::asContinue()        const { return ContinueStatement::cast(node); }
std::optional<ReturnStatement>       Statement::asReturn()          const { return ReturnStatement::cast(node); }
std::optional<ExpressionStatement>   Statement::asExpressionStmt()  const { return ExpressionStatement::cast(node); }
std::optional<ThrowStatement>        Statement::asThrow()           const { return ThrowStatement::cast(node); }
std::optional<RethrowStatement>      Statement::asRethrow()         const { return RethrowStatement::cast(node); }
std::optional<SwitchStatement>       Statement::asSwitch()          const { return SwitchStatement::cast(node); }

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
    // The only bare identifier token child is the variable name; identifiers in
    // the initializer are nested inside IdentExpr nodes.
    return firstChildNode(node, SyntaxKind::Identifier);
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

bool LetStatement::isConst() const {
    return firstChildNode(node, SyntaxKind::KwConst).has_value();
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

bool TypedVarDeclStatement::isConst() const {
    return firstChildNode(node, SyntaxKind::KwConst).has_value();
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

// === ForStatement ===

std::optional<Statement> ForStatement::init() const {
    for (auto& c : node.children()) {
        SyntaxKind k = c.kind();
        if (k == SyntaxKind::LetStmt || k == SyntaxKind::TypedVarDecl ||
            k == SyntaxKind::ExprStmt) {
            return Statement::cast(c);
        }
    }
    return std::nullopt;
}

std::optional<Expression> ForStatement::condition() const {
    // The init is wrapped in a statement node and the update in a ForUpdate node,
    // so the only direct expression child is the condition.
    for (auto& c : node.children()) {
        if (auto e = Expression::cast(c)) return e;
    }
    return std::nullopt;
}

std::optional<Expression> ForStatement::update() const {
    if (auto fu = firstChildNode(node, SyntaxKind::ForUpdate)) {
        for (auto& c : fu->children()) {
            if (auto e = Expression::cast(c)) return e;
        }
    }
    return std::nullopt;
}

std::optional<Block> ForStatement::body() const {
    if (auto b = firstChildNode(node, SyntaxKind::Block)) return Block::cast(*b);
    return std::nullopt;
}

// === ForEachStatement ===

std::optional<TypeReference> ForEachStatement::elementTypeRef() const {
    if (auto tr = firstChildNode(node, SyntaxKind::TypeRef)) return TypeReference::cast(*tr);
    return std::nullopt;
}

bool ForEachStatement::isLet() const {
    return firstChildNode(node, SyntaxKind::KwLet).has_value();
}

bool ForEachStatement::isConst() const {
    return firstChildNode(node, SyntaxKind::KwConst).has_value();
}

std::optional<SyntaxNode> ForEachStatement::elementNameToken() const {
    // The type name lives inside the TypeRef node, so the only direct identifier
    // token child is the element binding name.
    return firstChildNode(node, SyntaxKind::Identifier);
}

std::optional<std::u16string> ForEachStatement::elementNameText() const {
    if (auto t = elementNameToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}

std::optional<Expression> ForEachStatement::iterable() const {
    for (auto& c : node.children()) {
        if (auto e = Expression::cast(c)) return e;
    }
    return std::nullopt;
}

std::optional<Block> ForEachStatement::body() const {
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

// === SwitchStatement ===

std::optional<Expression> SwitchStatement::scrutinee() const {
    // Arms are SwitchArm nodes, so the only direct expression child is the
    // scrutinee.
    for (auto& c : node.children()) {
        if (auto e = Expression::cast(c)) return e;
    }
    return std::nullopt;
}

std::vector<SwitchArm> SwitchStatement::arms() const {
    std::vector<SwitchArm> out;
    for (auto& c : node.children()) {
        if (auto a = SwitchArm::cast(c)) out.push_back(*a);
    }
    return out;
}

}  // namespace ast
