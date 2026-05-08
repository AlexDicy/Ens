#include "Scope.h"

bool Scope::define(Symbol* s) {
    auto [_, inserted] = symbols.emplace(s->name, s);
    return inserted;
}

Symbol* Scope::lookupLocal(const std::u16string& name) const {
    auto it = symbols.find(name);
    return it == symbols.end() ? nullptr : it->second;
}

Symbol* Scope::lookup(const std::u16string& name) const {
    for (const Scope* s = this; s; s = s->parent) {
        auto it = s->symbols.find(name);
        if (it != s->symbols.end()) return it->second;
    }
    return nullptr;
}
