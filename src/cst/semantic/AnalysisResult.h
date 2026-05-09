#pragma once
#include <unordered_map>
#include "../Green.h"

class Type;
class Symbol;

namespace cst::semantic {

struct ResolutionInfo {
    Type* resolvedType = nullptr;
    Symbol* resolvedSymbol = nullptr;
    Symbol* resolvedMethodSymbol = nullptr;
};

class AnalysisResult {
public:
    ResolutionInfo* find(const GreenElement* g) {
        auto it = info.find(g);
        return it == info.end() ? nullptr : &it->second;
    }

    const ResolutionInfo* find(const GreenElement* g) const {
        auto it = info.find(g);
        return it == info.end() ? nullptr : &it->second;
    }

    ResolutionInfo& slot(const GreenElement* g) { return info[g]; }

    Type* typeOf(const GreenElement* g) const {
        auto it = info.find(g);
        return it == info.end() ? nullptr : it->second.resolvedType;
    }

    void setType(const GreenElement* g, Type* t) { info[g].resolvedType = t; }
    void setSymbol(const GreenElement* g, Symbol* s) { info[g].resolvedSymbol = s; }
    void setMethodSymbol(const GreenElement* g, Symbol* s) { info[g].resolvedMethodSymbol = s; }

private:
    std::unordered_map<const GreenElement*, ResolutionInfo> info;
};

}  // namespace cst::semantic
