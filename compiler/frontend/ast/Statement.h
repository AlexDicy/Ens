#pragma once
#include <optional>
#include <vector>
#include "../cst/SyntaxNode.h"
#include "Common.h"
#include "Expression.h"
#include "TypeReference.h"

namespace ast {

class Block;
class LetStatement;
class TypedVarDeclStatement;
class IfStatement;
class ElseClause;
class WhileStatement;
class ReturnStatement;
class ExpressionStatement;
class ThrowStatement;
class RethrowStatement;

class Statement {
public:
    SyntaxNode node;

    static bool isStatementKind(SyntaxKind k);
    static std::optional<Statement> cast(const SyntaxNode& n);

    SyntaxKind kind() const { return node.kind(); }

    std::optional<Block>                 asBlock() const;
    std::optional<LetStatement>          asLet() const;
    std::optional<TypedVarDeclStatement> asTypedVarDecl() const;
    std::optional<IfStatement>           asIf() const;
    std::optional<WhileStatement>        asWhile() const;
    std::optional<ReturnStatement>       asReturn() const;
    std::optional<ExpressionStatement>   asExpressionStmt() const;
    std::optional<ThrowStatement>        asThrow() const;
    std::optional<RethrowStatement>      asRethrow() const;
};

class Block {
public:
    SyntaxNode node;
    static std::optional<Block> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::Block) return std::nullopt;
        return Block{n};
    }
    std::vector<Statement> statements() const;
};

class LetStatement {
public:
    SyntaxNode node;
    static std::optional<LetStatement> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::LetStmt) return std::nullopt;
        return LetStatement{n};
    }
    std::optional<SyntaxNode> nameToken() const;
    std::optional<std::u16string> nameText() const;
    std::optional<Expression> initializer() const;
};

class TypedVarDeclStatement {
public:
    SyntaxNode node;
    static std::optional<TypedVarDeclStatement> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::TypedVarDecl) return std::nullopt;
        return TypedVarDeclStatement{n};
    }
    std::optional<TypeReference> typeReference() const;
    std::optional<SyntaxNode> nameToken() const;
    std::optional<std::u16string> nameText() const;
    std::optional<Expression> initializer() const;
};

class ElseClause {
public:
    SyntaxNode node;
    static std::optional<ElseClause> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ElseClause) return std::nullopt;
        return ElseClause{n};
    }
    std::optional<Block> block() const;
    std::optional<IfStatement> ifStatement() const;
};

class IfStatement {
public:
    SyntaxNode node;
    static std::optional<IfStatement> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::IfStmt) return std::nullopt;
        return IfStatement{n};
    }
    std::optional<Expression> condition() const;
    std::optional<Block> thenBlock() const;
    std::optional<ElseClause> elseClause() const;
};

class WhileStatement {
public:
    SyntaxNode node;
    static std::optional<WhileStatement> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::WhileStmt) return std::nullopt;
        return WhileStatement{n};
    }
    std::optional<Expression> condition() const;
    std::optional<Block> body() const;
};

class ReturnStatement {
public:
    SyntaxNode node;
    static std::optional<ReturnStatement> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ReturnStmt) return std::nullopt;
        return ReturnStatement{n};
    }
    std::optional<Expression> value() const;
};

class ExpressionStatement {
public:
    SyntaxNode node;
    static std::optional<ExpressionStatement> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ExprStmt) return std::nullopt;
        return ExpressionStatement{n};
    }
    std::optional<Expression> expression() const;
};

class ThrowStatement {
public:
    SyntaxNode node;
    static std::optional<ThrowStatement> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ThrowStmt) return std::nullopt;
        return ThrowStatement{n};
    }
    std::optional<Expression> value() const;
};

class RethrowStatement {
public:
    SyntaxNode node;
    static std::optional<RethrowStatement> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::RethrowStmt) return std::nullopt;
        return RethrowStatement{n};
    }
};

}  // namespace ast
