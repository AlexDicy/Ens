#pragma once
#include <optional>
#include <vector>
#include "../cst/SyntaxNode.h"
#include "Common.h"
#include "TypeReference.h"

namespace ast {

class LiteralExpression;
class IdentExpression;
class ThisExpression;
class BinaryExpression;
class PrefixExpression;
class PostfixExpression;
class CallExpression;
class MemberExpression;
class SafeMemberExpression;
class SubscriptExpression;
class OutArgument;
class AssignExpression;
class TernaryExpression;
class NewExpression;
class ParenExpression;
class ArgumentList;

class Expression {
public:
    SyntaxNode node;

    static bool isExpressionKind(SyntaxKind k);
    static std::optional<Expression> cast(const SyntaxNode& n);

    SyntaxKind kind() const { return node.kind(); }

    std::optional<LiteralExpression>   asLiteral() const;
    std::optional<IdentExpression>     asIdent() const;
    std::optional<ThisExpression>      asThis() const;
    std::optional<BinaryExpression>    asBinary() const;
    std::optional<PrefixExpression>    asPrefix() const;
    std::optional<PostfixExpression>   asPostfix() const;
    std::optional<CallExpression>      asCall() const;
    std::optional<MemberExpression>    asMember() const;
    std::optional<SafeMemberExpression> asSafeMember() const;
    std::optional<SubscriptExpression> asSubscript() const;
    std::optional<OutArgument>         asOutArgument() const;
    std::optional<AssignExpression>    asAssign() const;
    std::optional<TernaryExpression>   asTernary() const;
    std::optional<NewExpression>       asNew() const;
    std::optional<ParenExpression>     asParen() const;
};

class LiteralExpression {
public:
    SyntaxNode node;
    static std::optional<LiteralExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::LiteralExpr) return std::nullopt;
        return LiteralExpression{n};
    }
    std::optional<SyntaxNode> token() const;
    SyntaxKind literalKind() const;
};

class IdentExpression {
public:
    SyntaxNode node;
    static std::optional<IdentExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::IdentExpr) return std::nullopt;
        return IdentExpression{n};
    }
    std::optional<SyntaxNode> identifier() const;
    std::optional<std::u16string> nameText() const;
};

class ThisExpression {
public:
    SyntaxNode node;
    static std::optional<ThisExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ThisExpr) return std::nullopt;
        return ThisExpression{n};
    }
};

class BinaryExpression {
public:
    SyntaxNode node;
    static std::optional<BinaryExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::BinaryExpr) return std::nullopt;
        return BinaryExpression{n};
    }
    std::optional<Expression> left() const;
    std::optional<Expression> right() const;
    std::optional<SyntaxNode> operatorToken() const;
};

class PrefixExpression {
public:
    SyntaxNode node;
    static std::optional<PrefixExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::PrefixExpr) return std::nullopt;
        return PrefixExpression{n};
    }
    std::optional<Expression> operand() const;
    std::optional<SyntaxNode> operatorToken() const;
};

class PostfixExpression {
public:
    SyntaxNode node;
    static std::optional<PostfixExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::PostfixExpr) return std::nullopt;
        return PostfixExpression{n};
    }
    std::optional<Expression> operand() const;
    std::optional<SyntaxNode> operatorToken() const;
};

class ArgumentList {
public:
    SyntaxNode node;
    static std::optional<ArgumentList> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ArgList) return std::nullopt;
        return ArgumentList{n};
    }
    std::vector<Expression> arguments() const;
};

class CallExpression {
public:
    SyntaxNode node;
    static std::optional<CallExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::CallExpr) return std::nullopt;
        return CallExpression{n};
    }
    std::optional<Expression> callee() const;
    std::optional<ArgumentList> argumentList() const;
    std::vector<Expression> arguments() const;
};

class MemberExpression {
public:
    SyntaxNode node;
    static std::optional<MemberExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::MemberExpr) return std::nullopt;
        return MemberExpression{n};
    }
    std::optional<Expression> object() const;
    std::optional<SyntaxNode> memberName() const;
    std::optional<std::u16string> memberText() const;
};

class SafeMemberExpression {
public:
    SyntaxNode node;
    static std::optional<SafeMemberExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::SafeMemberExpr) return std::nullopt;
        return SafeMemberExpression{n};
    }
    std::optional<Expression> object() const;
    std::optional<SyntaxNode> memberName() const;
    std::optional<std::u16string> memberText() const;
};

class SubscriptExpression {
public:
    SyntaxNode node;
    static std::optional<SubscriptExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::SubscriptExpr) return std::nullopt;
        return SubscriptExpression{n};
    }
    std::optional<Expression> object() const;
    std::optional<Expression> index() const;
};

class AssignExpression {
public:
    SyntaxNode node;
    static std::optional<AssignExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::AssignExpr) return std::nullopt;
        return AssignExpression{n};
    }
    std::optional<Expression> target() const;
    std::optional<Expression> value() const;
    std::optional<SyntaxNode> operatorToken() const;
};

class TernaryExpression {
public:
    SyntaxNode node;
    static std::optional<TernaryExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::TernaryExpr) return std::nullopt;
        return TernaryExpression{n};
    }
    std::optional<Expression> condition() const;
    std::optional<Expression> thenBranch() const;
    std::optional<Expression> elseBranch() const;
};

class NewExpression {
public:
    SyntaxNode node;
    static std::optional<NewExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::NewExpr) return std::nullopt;
        return NewExpression{n};
    }
    std::optional<TypeReference> typeReference() const;
    std::optional<SyntaxNode> typeName() const;
    std::optional<std::u16string> typeNameText() const;
    std::optional<ArgumentList> argumentList() const;
    std::vector<Expression> arguments() const;
    bool isArrayNew() const;
    std::optional<Expression> arraySizeExpression() const;
};

class ParenExpression {
public:
    SyntaxNode node;
    static std::optional<ParenExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ParenExpr) return std::nullopt;
        return ParenExpression{n};
    }
    std::optional<Expression> inner() const;
};

class OutArgument {
public:
    SyntaxNode node;
    static std::optional<OutArgument> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::OutArgument) return std::nullopt;
        return OutArgument{n};
    }
    std::optional<SyntaxNode> identifier() const;
    std::optional<std::u16string> nameText() const;
};

}  // namespace ast
