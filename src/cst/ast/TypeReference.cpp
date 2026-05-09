#include "TypeReference.h"

namespace cst::ast {

std::optional<SyntaxNode> TypeReference::nameToken() const {
    for (auto& c : node.children()) {
        if (isTrivia(c.kind()) || !c.isToken()) continue;
        if (c.kind() == SyntaxKind::Identifier || isKeyword(c.kind())) return c;
    }
    return std::nullopt;
}

std::optional<std::u16string> TypeReference::nameText() const {
    if (auto t = nameToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}

bool TypeReference::isOptional() const {
    for (auto& c : node.children()) {
        if (c.kind() == SyntaxKind::Question) return true;
    }
    return false;
}

}  // namespace cst::ast
