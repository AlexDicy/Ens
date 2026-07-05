#pragma once
#include <string>
#include <vector>

class Type;
class GreenElement;
class Analyzer;
struct StructInfo;

enum class SymbolKind { Variable, Parameter, Function, Namespace, SiblingField };

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
    // For a Namespace symbol: the imported module, used to resolve qualified calls.
    const Analyzer* namespaceTarget = nullptr;

    // For a top-level function: whether it is reachable from other modules.
    bool isPublic = true;

    // Overrides the linker-level name when non-empty. Used by test declarations,
    // whose scope names ($test0, $test1, ...) repeat across modules.
    std::u16string linkName;

    // FFI metadata (set when this function symbol was declared via `external from "..."`).
    bool isExternal = false;
    std::vector<bool> paramIsOut;
    std::u16string libraryName;

    StructInfo* methodOwner = nullptr;

    // Generic function template: the type-parameter names and optional bounds.
    bool isTemplate = false;
    std::vector<std::u16string> typeParamNames;
    std::vector<StructInfo*> typeParamBounds;   // parallel to names; null = unbounded

    ParamEscapeInfo escapeInfo;

    // True for a `const` binding; reassigning it is a compile error.
    bool isConst = false;

    // True when this symbol surfaces an imported type name (not a runtime value),
    // letting member access tell `EnumType.Member` from `enumValue.member`.
    bool isTypeName = false;

    // escape/reassignment tracking for variables and parameters.
    EscapeKind localEscape = EscapeKind::NoEscape;
    bool reassigned = false;
    Symbol* aliasOf = nullptr;
    // true if every assignment has source = non-reassigned class parameter or `this`
    bool allAssignsFromParam = true;

    const GreenElement* lastUseRef = nullptr;
    bool lastUseInLoop = false;
    bool structFieldsMutated = false;

    bool stackPromoted = false;

    int siblingFieldIndex = -1;

    bool declaredThrows = false;
    // Empty for a bare `throws` whose set is inferred from the body.
    std::vector<StructInfo*> declaredThrowsTypes;
    // Computed outward throw set, sorted by pointer and deduped; keeps both a base and its subclass when both occur.
    std::vector<StructInfo*> throwsSet;
    // True if takes an error-slot parameter in the ABI.
    // For methods this is the root virtual declaration, so it is uniform across a vtable slot.
    bool abiThrows = false;

    Symbol(SymbolKind k, std::u16string n, Type* t, int l, int c)
        : kind(k), name(std::move(n)), type(t), line(l), column(c) {}
};
