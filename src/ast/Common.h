#pragma once
#include <optional>
#include <string>
#include <vector>
#include "../cst/SyntaxKind.h"
#include "../cst/SyntaxNode.h"

namespace ast {

inline std::optional<SyntaxNode> firstChildNode(const SyntaxNode& parent, SyntaxKind k) {
    for (auto& c : parent.children()) {
        if (!isTrivia(c.kind()) && c.kind() == k) return c;
    }
    return std::nullopt;
}

inline std::optional<SyntaxNode> firstNonTriviaToken(const SyntaxNode& parent) {
    for (auto& c : parent.children()) {
        if (!isTrivia(c.kind()) && c.isToken()) return c;
    }
    return std::nullopt;
}

inline std::optional<SyntaxNode> firstNonTriviaTokenIn(const SyntaxNode& parent,
                                                       std::initializer_list<SyntaxKind> kinds) {
    for (auto& c : parent.children()) {
        if (isTrivia(c.kind()) || !c.isToken()) continue;
        for (auto k : kinds) if (c.kind() == k) return c;
    }
    return std::nullopt;
}

inline std::vector<SyntaxNode> childrenOfKind(const SyntaxNode& parent, SyntaxKind k) {
    std::vector<SyntaxNode> out;
    for (auto& c : parent.children()) {
        if (c.kind() == k) out.push_back(c);
    }
    return out;
}

inline bool hasMissingOrError(const SyntaxNode& n) {
    if (n.kind() == SyntaxKind::Missing || n.kind() == SyntaxKind::Error) return true;
    for (auto& c : n.children()) {
        if (hasMissingOrError(c)) return true;
    }
    return false;
}

}  // namespace ast
