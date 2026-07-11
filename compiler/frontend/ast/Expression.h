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
class SuperExpression;
class BinaryExpression;
class PrefixExpression;
class PostfixExpression;
class CallExpression;
class MemberExpression;
class SafeMemberExpression;
class SubscriptExpression;
class SafeSubscriptExpression;
class CastExpression;
class CheckedCastExpression;
class TypeTestExpression;
class OutArgument;
class NamedArgument;
class AssignExpression;
class TernaryExpression;
class NullCoalesceExpression;
class NewExpression;
class ParenExpression;
class ArrayLiteralExpression;
class InterpStringExpression;
class TryExpression;
class SwitchArm;
class SwitchExpression;
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
    std::optional<SuperExpression>     asSuper() const;
    std::optional<BinaryExpression>    asBinary() const;
    std::optional<PrefixExpression>    asPrefix() const;
    std::optional<PostfixExpression>   asPostfix() const;
    std::optional<CallExpression>      asCall() const;
    std::optional<MemberExpression>    asMember() const;
    std::optional<SafeMemberExpression> asSafeMember() const;
    std::optional<SubscriptExpression> asSubscript() const;
    std::optional<SafeSubscriptExpression> asSafeSubscript() const;
    std::optional<CastExpression>      asCast() const;
    std::optional<CheckedCastExpression> asCheckedCast() const;
    std::optional<TypeTestExpression>  asTypeTest() const;
    std::optional<OutArgument>         asOutArgument() const;
    std::optional<NamedArgument>       asNamedArgument() const;
    std::optional<AssignExpression>    asAssign() const;
    std::optional<TernaryExpression>   asTernary() const;
    std::optional<NullCoalesceExpression> asNullCoalesce() const;
    std::optional<NewExpression>       asNew() const;
    std::optional<ParenExpression>     asParen() const;
    std::optional<ArrayLiteralExpression> asArrayLiteral() const;
    std::optional<InterpStringExpression> asInterpString() const;
    std::optional<TryExpression>       asTry() const;
    std::optional<SwitchExpression>    asSwitch() const;
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

class SuperExpression {
public:
    SyntaxNode node;
    static std::optional<SuperExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::SuperExpr) return std::nullopt;
        return SuperExpression{n};
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
    std::vector<TypeReference> typeArguments() const;
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

class SafeSubscriptExpression {
public:
    SyntaxNode node;
    static std::optional<SafeSubscriptExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::SafeSubscriptExpr) return std::nullopt;
        return SafeSubscriptExpression{n};
    }
    std::optional<Expression> object() const;
    std::optional<Expression> index() const;
};

class CastExpression {
public:
    SyntaxNode node;
    static std::optional<CastExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::CastExpr) return std::nullopt;
        return CastExpression{n};
    }
    std::optional<Expression> source() const;
    std::optional<TypeReference> targetType() const;
};

// A checked cast `value as? Type`: the value when the runtime type test
// succeeds, null when the value is null or not an instance of the target.
class CheckedCastExpression {
public:
    SyntaxNode node;
    static std::optional<CheckedCastExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::CheckedCastExpr) return std::nullopt;
        return CheckedCastExpression{n};
    }
    std::optional<Expression> source() const;
    std::optional<TypeReference> targetType() const;
};

// A type test `value is Type`, evaluating to bool.
class TypeTestExpression {
public:
    SyntaxNode node;
    static std::optional<TypeTestExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::TypeTestExpr) return std::nullopt;
        return TypeTestExpression{n};
    }
    std::optional<Expression> operand() const;
    std::optional<TypeReference> targetType() const;
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

class NullCoalesceExpression {
public:
    SyntaxNode node;
    static std::optional<NullCoalesceExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::NullCoalesceExpr) return std::nullopt;
        return NullCoalesceExpression{n};
    }
    std::optional<Expression> left() const;
    std::optional<Expression> right() const;
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
    std::vector<Expression> arraySizeExpressions() const;
    int arrayUnsizedTrailingCount() const;
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

class ArrayLiteralExpression {
public:
    SyntaxNode node;
    static std::optional<ArrayLiteralExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ArrayLiteralExpr) return std::nullopt;
        return ArrayLiteralExpression{n};
    }
    std::vector<Expression> elements() const;
};

class InterpStringExpression {
public:
    SyntaxNode node;
    static std::optional<InterpStringExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::InterpStringExpr) return std::nullopt;
        return InterpStringExpression{n};
    }
    // The literal text segments (Start, Mid..., End tokens), in source order.
    std::vector<SyntaxNode> parts() const;
    // The interpolated hole expressions, in source order.
    std::vector<Expression> holes() const;
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

// A `name: expr` call argument binding a parameter by name.
class NamedArgument {
public:
    SyntaxNode node;
    static std::optional<NamedArgument> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::NamedArgument) return std::nullopt;
        return NamedArgument{n};
    }
    std::optional<SyntaxNode> identifier() const;
    std::optional<std::u16string> nameText() const;
    std::optional<Expression> value() const;
};

class TryExpression {
public:
    SyntaxNode node;
    static std::optional<TryExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::TryExpr) return std::nullopt;
        return TryExpression{n};
    }
    std::optional<Expression> operand() const;
};

// One arm of a switch: `label[, label...] -> body`, `is Type [binding] -> body`,
// or `default -> body`. Shared by SwitchStatement and SwitchExpression. The
// body is either a block (statement form) or an expression.
class SwitchArm {
public:
    SyntaxNode node;
    static std::optional<SwitchArm> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::SwitchArm) return std::nullopt;
        return SwitchArm{n};
    }
    bool isDefault() const;
    // A type arm `is Type [binding] -> body`.
    bool isTypeArm() const;
    // The tested type of a type arm.
    std::optional<TypeReference> typeReference() const;
    // The optional binding name of a type arm.
    std::optional<SyntaxNode> bindingNameToken() const;
    std::optional<std::u16string> bindingNameText() const;
    // Label expressions before the `->` (empty for the default arm).
    std::vector<Expression> labels() const;
    // Body when it is an expression; the block node otherwise.
    std::optional<Expression> bodyExpr() const;
    std::optional<SyntaxNode> bodyBlockNode() const;
};

class SwitchExpression {
public:
    SyntaxNode node;
    static std::optional<SwitchExpression> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::SwitchExpr) return std::nullopt;
        return SwitchExpression{n};
    }
    std::optional<Expression> scrutinee() const;
    std::vector<SwitchArm> arms() const;
};

}  // namespace ast
