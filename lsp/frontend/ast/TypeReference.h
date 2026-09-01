#pragma once
#include <optional>
#include <string>
#include <vector>
#include "../cst/SyntaxNode.h"

namespace ast {

class TypeReference {
public:
    SyntaxNode node;

    static std::optional<TypeReference> cast(const SyntaxNode& n) {
        if (!isTypeNode(n.kind())) return std::nullopt;
        return TypeReference{n};
    }

    // The type of a function value: `(int, int) -> int`. Its parameter types and its
    // returned type are the nested types; a written `?` or `[]` binds to the returned
    // type instead, so a function type carries no suffix chain of its own.
    bool isFunctionType() const { return node.kind() == SyntaxKind::FuncType; }
    // A function type's own `throws Boom` clause, e.g. `(() -> void throws Boom) body`.
    bool hasThrowsClause() const;
    // A type in parentheses: `(T)`. It is what lets a suffix apply to a function type,
    // which is the only type it is allowed around.
    bool isParenthesized() const { return node.kind() == SyntaxKind::ParenType; }

    std::vector<SyntaxNode> segmentTokens() const;
    std::vector<std::u16string> pathSegments() const;
    std::optional<SyntaxNode> qualifierToken() const;
    std::optional<std::u16string> qualifierText() const;
    std::optional<SyntaxNode> nameToken() const;
    std::optional<std::u16string> nameText() const;
    std::vector<TypeReference> typeArguments() const;
    std::vector<TypeReference> parameterTypes() const;
    std::optional<TypeReference> returnedType() const;
    std::optional<TypeReference> innerType() const;
    bool isOptional() const;
    int arrayDepth() const;
    bool isArray() const { return arrayDepth() > 0; }

    // Suffix chain (in source order) wrapping the base type. Each entry is
    // either SyntaxKind::Question (T?) or SyntaxKind::LBracket (T[]).
    // `Box?[]` yields [Question, LBracket]; `Box[]?` yields [LBracket, Question].
    enum class Suffix { Optional, Array };
    std::vector<Suffix> suffixChain() const;
};

}  // namespace ast
