#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include "Symbol.h"

class Type;

// A single hop in a narrowing path: either a struct/class field access
// (`a.field`) or an array subscript (`a[index]`). For subscripts we
// distinguish by index kind so we can compare and invalidate precisely:
//   * `IntIndex`   - integer-literal index (e.g. xs[0])
//   * `IdentIndex` - plain identifier index (e.g. xs[i])
// Other index forms (arithmetic, calls, member reads) are NOT narrowable.
struct PathSegment {
    enum class Kind { Field, IntIndex, IdentIndex };
    Kind kind = Kind::Field;
    std::u16string field;            // when Kind == Field
    int64_t intIndex = 0;            // when Kind == IntIndex
    Symbol* identIndexSym = nullptr; // when Kind == IdentIndex

    bool operator==(const PathSegment& o) const {
        if (kind != o.kind) return false;
        switch (kind) {
            case Kind::Field:      return field == o.field;
            case Kind::IntIndex:   return intIndex == o.intIndex;
            case Kind::IdentIndex: return identIndexSym == o.identIndexSym;
        }
        return false;
    }
};

struct NarrowingPath {
    // `root` is either a Variable/Parameter Symbol or the synthetic `this` Symbol.
    Symbol* root = nullptr;
    std::vector<PathSegment> chain;

    bool operator==(const NarrowingPath& o) const {
        return root == o.root && chain == o.chain;
    }
};

struct NarrowingPathHash {
    size_t operator()(const NarrowingPath& p) const noexcept {
        size_t h = std::hash<Symbol*>{}(p.root);
        std::hash<std::u16string> sh;
        for (const auto& s : p.chain) {
            size_t segH = static_cast<size_t>(s.kind);
            switch (s.kind) {
                case PathSegment::Kind::Field:
                    segH ^= sh(s.field) * 1315423911u;
                    break;
                case PathSegment::Kind::IntIndex:
                    segH ^= std::hash<int64_t>{}(s.intIndex) * 2654435761u;
                    break;
                case PathSegment::Kind::IdentIndex:
                    segH ^= std::hash<Symbol*>{}(s.identIndexSym) * 0x9E3779B9u;
                    break;
            }
            h = h * 1315423911u ^ segH;
        }
        return h;
    }
};

class Scope {
public:
    Scope* parent = nullptr;
    std::unordered_map<std::u16string, Symbol*> symbols;
    std::unordered_map<NarrowingPath, Type*, NarrowingPathHash> narrowedTypes;

    explicit Scope(Scope* p = nullptr) : parent(p) {}

    bool define(Symbol* s);
    Symbol* lookup(const std::u16string& name) const;
    Symbol* lookupLocal(const std::u16string& name) const;

    Type* lookupNarrowedType(const NarrowingPath& key) const;

    void clearNarrowingsForRoot(Symbol* root);
    void clearNarrowingsAtOrBelow(const NarrowingPath& prefix);
    void clearNarrowingsForIndexSymbol(Symbol* sym);
};
