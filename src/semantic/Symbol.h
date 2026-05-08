#pragma once
#include <string>
#include <vector>

class Type;

enum class SymbolKind { Variable, Parameter, Function };

class Symbol {
public:
    SymbolKind kind;
    std::u16string name;
    Type* type = nullptr;
    int line = 0;
    int column = 0;

    std::vector<Type*> paramTypes;
    Type* returnType = nullptr;

    Symbol(SymbolKind k, std::u16string n, Type* t, int l, int c)
        : kind(k), name(std::move(n)), type(t), line(l), column(c) {}
};
