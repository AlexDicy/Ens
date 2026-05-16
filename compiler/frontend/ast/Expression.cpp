#include "Expression.h"

namespace ast {

bool Expression::isExpressionKind(SyntaxKind k) {
    switch (k) {
        case SyntaxKind::LiteralExpr:
        case SyntaxKind::IdentExpr:
        case SyntaxKind::ThisExpr:
        case SyntaxKind::BinaryExpr:
        case SyntaxKind::UnaryExpr:
        case SyntaxKind::PrefixExpr:
        case SyntaxKind::PostfixExpr:
        case SyntaxKind::CallExpr:
        case SyntaxKind::MemberExpr:
        case SyntaxKind::SafeMemberExpr:
        case SyntaxKind::SubscriptExpr:
        case SyntaxKind::AssignExpr:
        case SyntaxKind::TernaryExpr:
        case SyntaxKind::NewExpr:
        case SyntaxKind::ParenExpr:
            return true;
        default:
            return false;
    }
}

std::optional<Expression> Expression::cast(const SyntaxNode& n) {
    if (!isExpressionKind(n.kind())) return std::nullopt;
    return Expression{n};
}

static std::optional<Expression> firstExpressionChild(const SyntaxNode& parent) {
    for (auto& c : parent.children()) {
        if (auto e = Expression::cast(c)) return e;
    }
    return std::nullopt;
}

static std::vector<Expression> expressionChildren(const SyntaxNode& parent) {
    std::vector<Expression> out;
    for (auto& c : parent.children()) {
        if (auto e = Expression::cast(c)) out.push_back(*e);
    }
    return out;
}

std::optional<LiteralExpression>   Expression::asLiteral()   const { return LiteralExpression::cast(node); }
std::optional<IdentExpression>     Expression::asIdent()     const { return IdentExpression::cast(node); }
std::optional<ThisExpression>      Expression::asThis()      const { return ThisExpression::cast(node); }
std::optional<BinaryExpression>    Expression::asBinary()    const { return BinaryExpression::cast(node); }
std::optional<PrefixExpression>    Expression::asPrefix()    const { return PrefixExpression::cast(node); }
std::optional<PostfixExpression>   Expression::asPostfix()   const { return PostfixExpression::cast(node); }
std::optional<CallExpression>      Expression::asCall()      const { return CallExpression::cast(node); }
std::optional<MemberExpression>    Expression::asMember()    const { return MemberExpression::cast(node); }
std::optional<SafeMemberExpression> Expression::asSafeMember() const { return SafeMemberExpression::cast(node); }
std::optional<SubscriptExpression> Expression::asSubscript() const { return SubscriptExpression::cast(node); }
std::optional<AssignExpression>    Expression::asAssign()    const { return AssignExpression::cast(node); }
std::optional<TernaryExpression>   Expression::asTernary()   const { return TernaryExpression::cast(node); }
std::optional<NewExpression>       Expression::asNew()       const { return NewExpression::cast(node); }
std::optional<ParenExpression>     Expression::asParen()     const { return ParenExpression::cast(node); }

// === LiteralExpression ===

std::optional<SyntaxNode> LiteralExpression::token() const {
    for (auto& c : node.children()) {
        if (isTrivia(c.kind()) || !c.isToken()) continue;
        if (isLiteral(c.kind())) return c;
    }
    return std::nullopt;
}

SyntaxKind LiteralExpression::literalKind() const {
    if (auto t = token()) return t->kind();
    return SyntaxKind::Invalid;
}

// === IdentExpression ===

std::optional<SyntaxNode> IdentExpression::identifier() const {
    for (auto& c : node.children()) {
        if (!isTrivia(c.kind()) && c.kind() == SyntaxKind::Identifier) return c;
    }
    return std::nullopt;
}

std::optional<std::u16string> IdentExpression::nameText() const {
    if (auto t = identifier()) return std::u16string(t->tokenText());
    return std::nullopt;
}

// === BinaryExpression ===

std::optional<Expression> BinaryExpression::left() const {
    return firstExpressionChild(node);
}

std::optional<Expression> BinaryExpression::right() const {
    auto exprs = expressionChildren(node);
    if (exprs.size() >= 2) return exprs[1];
    return std::nullopt;
}

std::optional<SyntaxNode> BinaryExpression::operatorToken() const {
    bool seenLeft = false;
    for (auto& c : node.children()) {
        if (isTrivia(c.kind())) continue;
        if (Expression::isExpressionKind(c.kind())) {
            if (!seenLeft) { seenLeft = true; continue; }
            break;
        }
        if (seenLeft && c.isToken()) return c;
    }
    return std::nullopt;
}

// === PrefixExpression / PostfixExpression ===

std::optional<Expression> PrefixExpression::operand() const {
    return firstExpressionChild(node);
}

std::optional<SyntaxNode> PrefixExpression::operatorToken() const {
    return firstNonTriviaToken(node);
}

std::optional<Expression> PostfixExpression::operand() const {
    return firstExpressionChild(node);
}

std::optional<SyntaxNode> PostfixExpression::operatorToken() const {
    bool seenExpr = false;
    for (auto& c : node.children()) {
        if (isTrivia(c.kind())) continue;
        if (Expression::isExpressionKind(c.kind())) { seenExpr = true; continue; }
        if (seenExpr && c.isToken()) return c;
    }
    return std::nullopt;
}

// === ArgumentList ===

std::vector<Expression> ArgumentList::arguments() const {
    return expressionChildren(node);
}

// === CallExpression ===

std::optional<Expression> CallExpression::callee() const {
    return firstExpressionChild(node);
}

std::optional<ArgumentList> CallExpression::argumentList() const {
    auto a = firstChildNode(node, SyntaxKind::ArgList);
    if (!a) return std::nullopt;
    return ArgumentList::cast(*a);
}

std::vector<Expression> CallExpression::arguments() const {
    if (auto al = argumentList()) return al->arguments();
    return {};
}

// === MemberExpression ===

std::optional<Expression> MemberExpression::object() const {
    return firstExpressionChild(node);
}

std::optional<SyntaxNode> MemberExpression::memberName() const {
    bool seenDot = false;
    for (auto& c : node.children()) {
        if (isTrivia(c.kind())) continue;
        if (c.kind() == SyntaxKind::Dot) { seenDot = true; continue; }
        if (seenDot && c.kind() == SyntaxKind::Identifier) return c;
    }
    return std::nullopt;
}

std::optional<std::u16string> MemberExpression::memberText() const {
    if (auto t = memberName()) return std::u16string(t->tokenText());
    return std::nullopt;
}

// === SafeMemberExpression ===

std::optional<Expression> SafeMemberExpression::object() const {
    return firstExpressionChild(node);
}

std::optional<SyntaxNode> SafeMemberExpression::memberName() const {
    bool seenOp = false;
    for (auto& c : node.children()) {
        if (isTrivia(c.kind())) continue;
        if (c.kind() == SyntaxKind::QuestionDot) { seenOp = true; continue; }
        if (seenOp && c.kind() == SyntaxKind::Identifier) return c;
    }
    return std::nullopt;
}

std::optional<std::u16string> SafeMemberExpression::memberText() const {
    if (auto t = memberName()) return std::u16string(t->tokenText());
    return std::nullopt;
}

// === SubscriptExpression ===

std::optional<Expression> SubscriptExpression::object() const {
    return firstExpressionChild(node);
}

std::optional<Expression> SubscriptExpression::index() const {
    auto exprs = expressionChildren(node);
    if (exprs.size() >= 2) return exprs[1];
    return std::nullopt;
}

// === AssignExpression ===

std::optional<Expression> AssignExpression::target() const {
    return firstExpressionChild(node);
}

std::optional<Expression> AssignExpression::value() const {
    auto exprs = expressionChildren(node);
    if (exprs.size() >= 2) return exprs[1];
    return std::nullopt;
}

std::optional<SyntaxNode> AssignExpression::operatorToken() const {
    bool seenTarget = false;
    for (auto& c : node.children()) {
        if (isTrivia(c.kind())) continue;
        if (Expression::isExpressionKind(c.kind())) {
            if (!seenTarget) { seenTarget = true; continue; }
            break;
        }
        if (seenTarget && c.isToken()) return c;
    }
    return std::nullopt;
}

// === TernaryExpression ===

std::optional<Expression> TernaryExpression::condition() const {
    return firstExpressionChild(node);
}

std::optional<Expression> TernaryExpression::thenBranch() const {
    auto exprs = expressionChildren(node);
    if (exprs.size() >= 2) return exprs[1];
    return std::nullopt;
}

std::optional<Expression> TernaryExpression::elseBranch() const {
    auto exprs = expressionChildren(node);
    if (exprs.size() >= 3) return exprs[2];
    return std::nullopt;
}

// === NewExpression ===

std::optional<TypeReference> NewExpression::typeReference() const {
    if (auto tr = firstChildNode(node, SyntaxKind::TypeRef)) return TypeReference::cast(*tr);
    return std::nullopt;
}

std::optional<SyntaxNode> NewExpression::typeName() const {
    if (auto tr = typeReference()) return tr->nameToken();
    return std::nullopt;
}

std::optional<std::u16string> NewExpression::typeNameText() const {
    if (auto tr = typeReference()) return tr->nameText();
    return std::nullopt;
}

std::optional<ArgumentList> NewExpression::argumentList() const {
    auto a = firstChildNode(node, SyntaxKind::ArgList);
    if (!a) return std::nullopt;
    return ArgumentList::cast(*a);
}

std::vector<Expression> NewExpression::arguments() const {
    if (auto al = argumentList()) return al->arguments();
    return {};
}

// === ParenExpression ===

std::optional<Expression> ParenExpression::inner() const {
    return firstExpressionChild(node);
}

}  // namespace ast
