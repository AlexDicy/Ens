#pragma once
#include <string>
#include <vector>

class Type;
class GreenElement;

enum class SymbolKind { Variable, Parameter, Function, Namespace };

class Symbol {
public:
    SymbolKind kind;
    std::u16string name;
    Type* type = nullptr;
    int line = 0;
    int column = 0;
    bool isBuiltin = false;

    std::vector<Type*> paramTypes;
    Type* returnType = nullptr;
    const GreenElement* funcDeclCst = nullptr;

    std::u16string namespaceModulePath;

    Symbol(SymbolKind k, std::u16string n, Type* t, int l, int c)
        : kind(k), name(std::move(n)), type(t), line(l), column(c) {}
};
