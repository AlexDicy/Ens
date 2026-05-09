#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include "Green.h"
#include "SyntaxKind.h"

class SyntaxBuilder {
public:
    SyntaxBuilder();

    void startNode(SyntaxKind kind);
    void token(SyntaxKind kind, std::u16string text);
    void finishNode();

    // Returns an opaque marker that records the position before the most recent
    // child so a node can be inserted retroactively (Pratt-style infix wrapping).
    size_t checkpoint() const;
    void startNodeAt(size_t checkpoint, SyntaxKind kind);

    GreenElementPtr build();

private:
    struct Frame {
        SyntaxKind kind;
        std::vector<GreenElementPtr> children;
    };
    std::vector<Frame> stack;
};
