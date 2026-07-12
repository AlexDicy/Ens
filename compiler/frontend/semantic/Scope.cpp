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

void Scope::clearNarrowingsForRoot(Symbol* root) {
    for (Scope* s = this; s; s = s->parent) {
        for (auto it = s->narrowedTypes.begin(); it != s->narrowedTypes.end();) {
            if (it->first.root == root) it = s->narrowedTypes.erase(it);
            else ++it;
        }
    }
}

// Erases the narrowings that pass through `root` but keeps the narrowing of
// the binding itself.
void Scope::clearNarrowingsForRootMembers(Symbol* root) {
    for (Scope* s = this; s; s = s->parent) {
        for (auto it = s->narrowedTypes.begin(); it != s->narrowedTypes.end();) {
            if (it->first.root == root && !it->first.chain.empty()) {
                it = s->narrowedTypes.erase(it);
            } else {
                ++it;
            }
        }
    }
}

static bool startsWith(const std::vector<PathSegment>& chain,
                       const std::vector<PathSegment>& prefix) {
    if (chain.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (!(chain[i] == prefix[i])) return false;
    }
    return true;
}

void Scope::clearNarrowingsAtOrBelow(const NarrowingPath& prefix) {
    for (Scope* s = this; s; s = s->parent) {
        for (auto it = s->narrowedTypes.begin(); it != s->narrowedTypes.end();) {
            const NarrowingPath& key = it->first;
            if (key.root == prefix.root && startsWith(key.chain, prefix.chain)) {
                it = s->narrowedTypes.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void Scope::clearNarrowingsForIndexSymbol(Symbol* sym) {
    if (!sym) return;
    for (Scope* s = this; s; s = s->parent) {
        for (auto it = s->narrowedTypes.begin(); it != s->narrowedTypes.end();) {
            bool hit = false;
            for (const auto& seg : it->first.chain) {
                if (seg.kind == PathSegment::Kind::IdentIndex && seg.identIndexSym == sym) {
                    hit = true;
                    break;
                }
            }
            if (hit) it = s->narrowedTypes.erase(it);
            else ++it;
        }
    }
}
