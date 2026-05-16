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

Type* Scope::lookupNarrowedType(const NarrowingPath& key) const {
    for (const Scope* s = this; s; s = s->parent) {
        auto it = s->narrowedTypes.find(key);
        if (it != s->narrowedTypes.end()) return it->second;
    }
    return nullptr;
}

void Scope::clearNarrowingsContaining(Symbol* root, const std::u16string& field) {
    for (Scope* s = this; s; s = s->parent) {
        for (auto it = s->narrowedTypes.begin(); it != s->narrowedTypes.end();) {
            const NarrowingPath& key = it->first;
            bool matches = (key.root == root);
            if (matches && !field.empty()) {
                matches = !key.fieldChain.empty() && key.fieldChain.front() == field;
            }
            if (matches) it = s->narrowedTypes.erase(it);
            else ++it;
        }
    }
}
