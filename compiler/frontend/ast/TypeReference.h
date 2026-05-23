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
        if (n.kind() != SyntaxKind::TypeRef) return std::nullopt;
        return TypeReference{n};
    }

    std::vector<SyntaxNode> segmentTokens() const;
    std::vector<std::u16string> pathSegments() const;
    std::optional<SyntaxNode> qualifierToken() const;
    std::optional<std::u16string> qualifierText() const;
    std::optional<SyntaxNode> nameToken() const;
    std::optional<std::u16string> nameText() const;
    bool isOptional() const;
    int arrayDepth() const;
    bool isArray() const { return arrayDepth() > 0; }
};

}  // namespace ast
