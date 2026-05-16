#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include "Symbol.h"

class Type;

struct NarrowingPath {
    Symbol* root = nullptr;
    std::vector<std::u16string> fieldChain;

    bool operator==(const NarrowingPath& o) const {
        return root == o.root && fieldChain == o.fieldChain;
    }
};

struct NarrowingPathHash {
    size_t operator()(const NarrowingPath& p) const noexcept {
        size_t h = std::hash<Symbol*>{}(p.root);
        std::hash<std::u16string> sh;
        for (const auto& s : p.fieldChain) {
            h = h * 1315423911u ^ sh(s);
        }
        return h;
    }
};

class Scope {
public:
    Scope* parent = nullptr;
    std::unordered_map<std::u16string, Symbol*> symbols;
    std::unordered_map<NarrowingPath, Type*, NarrowingPathHash> narrowedTypes;

    explicit Scope(Scope* p = nullptr) : parent(p) {}

    bool define(Symbol* s);
    Symbol* lookup(const std::u16string& name) const;
    Symbol* lookupLocal(const std::u16string& name) const;

    Type* lookupNarrowedType(const NarrowingPath& key) const;
    void clearNarrowingsContaining(Symbol* root, const std::u16string& field);
};
