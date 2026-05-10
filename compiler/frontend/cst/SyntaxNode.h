#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <vector>
#include "Green.h"
#include "SyntaxKind.h"

class SyntaxNode {
public:
    SyntaxNode(const GreenElement* green, const SyntaxNode* parent, uint32_t offset, uint32_t indexInParent);

    SyntaxKind kind() const { return green->kind; }
    uint32_t startOffset() const { return offset; }
    uint32_t endOffset() const { return offset + green->length; }
    uint32_t length() const { return green->length; }
    const SyntaxNode* getParent() const { return parent; }

    bool isToken() const { return green->isToken(); }
    bool isNode() const { return !isToken(); }

    std::u16string_view tokenText() const;

    std::vector<SyntaxNode> children() const;
    std::vector<SyntaxNode> nonTriviaChildren() const;

    std::optional<SyntaxNode> firstChild(SyntaxKind k) const;
    std::optional<SyntaxNode> firstToken(SyntaxKind k) const;

    std::u16string fullText() const;

    void dump(std::ostream& os, int indent = 0) const;

    static std::unique_ptr<SyntaxNode> makeRoot(const GreenElement* green) {
        return std::unique_ptr<SyntaxNode>(new SyntaxNode(green, nullptr, 0, 0));
    }

    const GreenElement* greenNode() const { return green; }

    std::optional<SyntaxNode> tokenAtOffset(uint32_t pos) const;
    std::optional<SyntaxNode> nodeCovering(uint32_t start, uint32_t end) const;

private:
    const GreenElement* green;
    const SyntaxNode* parent;
    uint32_t offset;
    uint32_t indexInParent;
};
