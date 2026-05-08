#pragma once
#include <ostream>

class Node {
public:
    int line = 0;
    int column = 0;
    virtual ~Node() = default;
    virtual void dump(std::ostream& os, int indent) const = 0;

protected:
    static void writeIndent(std::ostream& os, int indent) {
        for (int i = 0; i < indent; ++i) os << "  ";
    }
};
