#pragma once
#include <memory>
#include <string>
#include "Node.h"

class TypeNode : public Node {
public:
    std::u16string name;
    bool isOptional;

    TypeNode(std::u16string n, bool opt) : name(std::move(n)), isOptional(opt) {}
    void dump(std::ostream& os, int indent) const override;
};

using TypePtr = std::unique_ptr<TypeNode>;
