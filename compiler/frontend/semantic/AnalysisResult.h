#pragma once
#include <unordered_map>
#include "../cst/Green.h"

class Type;
class Symbol;

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

    Type* receiverOf(const GreenElement* fnDecl) const {
        auto it = methodReceivers.find(fnDecl);
        return it == methodReceivers.end() ? nullptr : it->second;
    }
    void setReceiver(const GreenElement* fnDecl, Type* t) { methodReceivers[fnDecl] = t; }

    Symbol* thisSymbolOf(const GreenElement* fnDecl) const {
        auto it = thisSymbols.find(fnDecl);
        return it == thisSymbols.end() ? nullptr : it->second;
    }
    void setThisSymbol(const GreenElement* fnDecl, Symbol* sym) { thisSymbols[fnDecl] = sym; }

private:
    std::unordered_map<const GreenElement*, ResolutionInfo> info;
    std::unordered_map<const GreenElement*, Type*> methodReceivers;
    std::unordered_map<const GreenElement*, Symbol*> thisSymbols;
};
