#include "TypeReference.h"

namespace ast {

std::vector<SyntaxNode> TypeReference::segmentTokens() const {
    std::vector<SyntaxNode> out;
    for (auto& c : node.children()) {
        if (isTrivia(c.kind()) || !c.isToken()) continue;
        if (c.kind() == SyntaxKind::Identifier || isKeyword(c.kind())) out.push_back(c);
    }
    return out;
}

std::vector<std::u16string> TypeReference::pathSegments() const {
    std::vector<std::u16string> out;
    for (auto& t : segmentTokens()) out.emplace_back(t.tokenText());
    return out;
}

std::optional<SyntaxNode> TypeReference::qualifierToken() const {
    auto tokens = segmentTokens();
    if (tokens.size() < 2) return std::nullopt;
    return tokens.front();
}

std::optional<std::u16string> TypeReference::qualifierText() const {
    if (auto t = qualifierToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}

std::optional<SyntaxNode> TypeReference::nameToken() const {
    auto tokens = segmentTokens();
    if (tokens.empty()) return std::nullopt;
    return tokens.back();
}

std::optional<std::u16string> TypeReference::nameText() const {
    if (auto t = nameToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}

std::vector<TypeReference> TypeReference::typeArguments() const {
    std::vector<TypeReference> out;
    for (auto& c : node.children()) {
        if (c.kind() != SyntaxKind::TypeArgList) continue;
        for (auto& a : c.children()) {
            if (auto tr = TypeReference::cast(a)) out.push_back(*tr);
        }
        break;
    }
    return out;
}

bool TypeReference::isOptional() const {
    for (auto& c : node.children()) {
        if (c.kind() == SyntaxKind::Question) return true;
    }
    return false;
}

int TypeReference::arrayDepth() const {
    int depth = 0;
    for (auto& c : node.children()) {
        if (c.kind() == SyntaxKind::LBracket) depth++;
    }
    return depth;
}

std::vector<TypeReference::Suffix> TypeReference::suffixChain() const {
    // Suffix tokens (Question / LBracket) only appear after the name segments
    // in a well-formed TypeRef, so a single in-order pass is enough.
    std::vector<Suffix> out;
    for (auto& c : node.children()) {
        if (isTrivia(c.kind())) continue;
        if (c.kind() == SyntaxKind::Question) {
            out.push_back(Suffix::Optional);
        } else if (c.kind() == SyntaxKind::LBracket) {
            out.push_back(Suffix::Array);
        }
    }
    return out;
}

}  // namespace ast
