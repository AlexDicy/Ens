#pragma once
#include <string>
#include <vector>

class Type;
class GreenElement;

enum class SymbolKind { Variable, Parameter, Function, Namespace };

enum class EscapeKind : unsigned char {
    Unknown,
    NoEscape,
    Escape,
};

struct ParamEscapeInfo {
    std::vector<EscapeKind> params;
    std::vector<bool> paramMutated;
    EscapeKind returnEscape = EscapeKind::Unknown;
    bool analyzed = false;
};

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

    ParamEscapeInfo escapeInfo;

    // escape/reassignment tracking for variables and parameters.
    EscapeKind localEscape = EscapeKind::NoEscape;
    bool reassigned = false;
    Symbol* aliasOf = nullptr;
    // true if every assignment has source = non-reassigned class parameter or `this`
    bool allAssignsFromParam = true;

    const GreenElement* lastUseRef = nullptr;
    bool lastUseInLoop = false;

    Symbol(SymbolKind k, std::u16string n, Type* t, int l, int c)
        : kind(k), name(std::move(n)), type(t), line(l), column(c) {}
};
