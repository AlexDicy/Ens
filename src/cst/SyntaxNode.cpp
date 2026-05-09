#include "SyntaxNode.h"

SyntaxNode::SyntaxNode(const GreenElement* g, const SyntaxNode* p, uint32_t off, uint32_t idx)
    : green(g), parent(p), offset(off), indexInParent(idx) {}

std::u16string_view SyntaxNode::tokenText() const {
    if (auto* t = dynamic_cast<const GreenToken*>(green)) {
        return t->text;
    }
    return {};
}

std::vector<SyntaxNode> SyntaxNode::children() const {
    std::vector<SyntaxNode> out;
    if (auto* n = dynamic_cast<const GreenNode*>(green)) {
        uint32_t childOffset = offset;
        for (uint32_t i = 0; i < n->children.size(); ++i) {
            auto* c = n->children[i].get();
            out.emplace_back(c, this, childOffset, i);
            childOffset += c->length;
        }
    }
    return out;
}

std::vector<SyntaxNode> SyntaxNode::nonTriviaChildren() const {
    std::vector<SyntaxNode> out;
    for (auto& c : children()) {
        if (!isTrivia(c.kind())) out.push_back(c);
    }
    return out;
}

std::optional<SyntaxNode> SyntaxNode::firstChild(SyntaxKind k) const {
    for (auto& c : children()) {
        if (c.kind() == k) return c;
    }
    return std::nullopt;
}

std::optional<SyntaxNode> SyntaxNode::firstToken(SyntaxKind k) const {
    for (auto& c : children()) {
        if (c.isToken() && c.kind() == k) return c;
    }
    return std::nullopt;
}

std::u16string SyntaxNode::fullText() const {
    if (auto* t = dynamic_cast<const GreenToken*>(green)) return t->text;
    std::u16string buf;
    for (auto& c : children()) buf += c.fullText();
    return buf;
}

static std::string asciiOf(std::u16string_view s) {
    std::string r;
    r.reserve(s.size());
    for (char16_t c : s) {
        if (c == u'\n') { r += "\\n"; continue; }
        if (c == u'\r') { r += "\\r"; continue; }
        if (c == u'\t') { r += "\\t"; continue; }
        r.push_back(c < 128 ? static_cast<char>(c) : '?');
    }
    return r;
}

void SyntaxNode::dump(std::ostream& os, int indent) const {
    for (int i = 0; i < indent; ++i) os << "  ";
    os << kindName(kind()) << " @" << offset << ".." << endOffset();
    if (auto* t = dynamic_cast<const GreenToken*>(green)) {
        os << " \"" << asciiOf(t->text) << "\"";
    }
    os << "\n";
    for (auto& c : children()) c.dump(os, indent + 1);
}

std::optional<SyntaxNode> SyntaxNode::tokenAtOffset(uint32_t pos) const {
    if (pos < offset || pos > endOffset()) return std::nullopt;
    if (isToken()) return *this;
    for (auto& c : children()) {
        if (pos >= c.startOffset() && pos < c.endOffset()) {
            return c.tokenAtOffset(pos);
        }
    }
    return std::nullopt;
}

std::optional<SyntaxNode> SyntaxNode::nodeCovering(uint32_t start, uint32_t end) const {
    if (start < offset || end > endOffset()) return std::nullopt;
    for (auto& c : children()) {
        if (start >= c.startOffset() && end <= c.endOffset()) {
            if (auto inner = c.nodeCovering(start, end)) return inner;
            return c;
        }
    }
    return *this;
}
