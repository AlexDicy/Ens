#include "SyntaxBuilder.h"

#include <cassert>
#include <utility>

SyntaxBuilder::SyntaxBuilder() = default;

void SyntaxBuilder::startNode(SyntaxKind kind) {
    stack.push_back(Frame{kind, {}});
}

void SyntaxBuilder::token(SyntaxKind kind, std::u16string text) {
    assert(!stack.empty() && "token() with no enclosing node");
    stack.back().children.emplace_back(std::make_unique<GreenToken>(kind, std::move(text)));
}

void SyntaxBuilder::finishNode() {
    assert(!stack.empty() && "finishNode() with empty stack");
    auto frame = std::move(stack.back());
    stack.pop_back();
    auto node = std::make_unique<GreenNode>(frame.kind, std::move(frame.children));
    if (stack.empty()) {
        // Root finished; push back as a one-frame holder so build() can extract it.
        stack.push_back(Frame{frame.kind, {}});
        stack.back().children.emplace_back(std::move(node));
    } else {
        stack.back().children.emplace_back(std::move(node));
    }
}

size_t SyntaxBuilder::checkpoint() const {
    assert(!stack.empty() && "checkpoint() with empty stack");
    return stack.back().children.size();
}

void SyntaxBuilder::startNodeAt(size_t checkpoint, SyntaxKind kind) {
    assert(!stack.empty() && "startNodeAt() with empty stack");
    auto& frame = stack.back();
    assert(checkpoint <= frame.children.size());
    Frame newFrame{kind, {}};
    newFrame.children.reserve(frame.children.size() - checkpoint);
    for (size_t i = checkpoint; i < frame.children.size(); ++i) {
        newFrame.children.emplace_back(std::move(frame.children[i]));
    }
    frame.children.resize(checkpoint);
    stack.push_back(std::move(newFrame));
}

GreenElementPtr SyntaxBuilder::build() {
    assert(stack.size() == 1 && stack.back().children.size() == 1
           && "build() requires exactly one finished root");
    auto root = std::move(stack.back().children.front());
    stack.clear();
    return root;
}
