#include "Green.h"

GreenNode::GreenNode(SyntaxKind k, std::vector<GreenElementPtr> ch)
    : GreenElement(k, 0), children(std::move(ch)) {
    uint32_t total = 0;
    for (auto& c : children) total += c->length;
    length = total;
}
