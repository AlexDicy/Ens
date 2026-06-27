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
class ForStatement;
class ForEachStatement;
class BreakStatement;
class ContinueStatement;
class ReturnStatement;
class ExpressionStatement;
class ThrowStatement;
class RethrowStatement;
class SwitchStatement;

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
    std::optional<ForStatement>          asFor() const;
    std::optional<ForEachStatement>      asForEach() const;
    std::optional<BreakStatement>        asBreak() const;
    std::optional<ContinueStatement>     asContinue() const;
    std::optional<ReturnStatement>       asReturn() const;
    std::optional<ExpressionStatement>   asExpressionStmt() const;
    std::optional<ThrowStatement>        asThrow() const;
    std::optional<RethrowStatement>      asRethrow() const;
    std::optional<SwitchStatement>       asSwitch() const;
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
    bool isConst() const;
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
    bool isConst() const;
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

class ForStatement {
public:
    SyntaxNode node;
    static std::optional<ForStatement> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ForStmt) return std::nullopt;
        return ForStatement{n};
    }
    std::optional<Statement> init() const;
    std::optional<Expression> condition() const;
    std::optional<Expression> update() const;
    std::optional<Block> body() const;
};

class ForEachStatement {
public:
    SyntaxNode node;
    static std::optional<ForEachStatement> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ForEachStmt) return std::nullopt;
        return ForEachStatement{n};
    }
    std::optional<TypeReference> elementTypeRef() const;
    bool isLet() const;
    bool isConst() const;
    std::optional<SyntaxNode> elementNameToken() const;
    std::optional<std::u16string> elementNameText() const;
    std::optional<Expression> iterable() const;
    std::optional<Block> body() const;
};

class BreakStatement {
public:
    SyntaxNode node;
    static std::optional<BreakStatement> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::BreakStmt) return std::nullopt;
        return BreakStatement{n};
    }
};

class ContinueStatement {
public:
    SyntaxNode node;
    static std::optional<ContinueStatement> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ContinueStmt) return std::nullopt;
        return ContinueStatement{n};
    }
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

class SwitchStatement {
public:
    SyntaxNode node;
    static std::optional<SwitchStatement> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::SwitchStmt) return std::nullopt;
        return SwitchStatement{n};
    }
    std::optional<Expression> scrutinee() const;
    std::vector<SwitchArm> arms() const;
};

}  // namespace ast
