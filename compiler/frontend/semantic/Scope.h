#pragma once
#include <string>
#include <unordered_map>
#include "Symbol.h"

class Scope {
public:
    Scope* parent = nullptr;
    std::unordered_map<std::u16string, Symbol*> symbols;

    explicit Scope(Scope* p = nullptr) : parent(p) {}

    bool define(Symbol* s);
    Symbol* lookup(const std::u16string& name) const;
    Symbol* lookupLocal(const std::u16string& name) const;
};
