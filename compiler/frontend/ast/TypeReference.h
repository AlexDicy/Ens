#pragma once
#include <optional>
#include "../cst/SyntaxNode.h"
#include "Common.h"

namespace ast {

class TypeReference {
public:
    SyntaxNode node;

    static std::optional<TypeReference> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::TypeRef) return std::nullopt;
        return TypeReference{n};
    }

    std::optional<SyntaxNode> nameToken() const;
    std::optional<std::u16string> nameText() const;
    bool isOptional() const;
};

}  // namespace ast
