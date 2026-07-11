#pragma once
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "Symbol.h"
#include "Type.h"

class TypeContext {
public:
    TypeContext();

    Type* getPrimitive(TypeKind k);
    Type* getOptional(Type* inner);
    Type* getArray(Type* element);
    Type* getError() { return errorType; }
    Type* getNull() { return nullType; }

    Type* primitiveFromName(const std::u16string& name);

    Type* registerStruct(const std::u16string& modulePath, std::u16string name);
    Type* lookupStruct(const std::u16string& modulePath, const std::u16string& name) const;

    Type* registerClass(const std::u16string& modulePath, std::u16string name);
    Type* lookupClass(const std::u16string& modulePath, const std::u16string& name) const;

    // An interface is a class-kind type flagged isInterface; it shares the
    // class namespace so name lookups treat the two uniformly.
    Type* registerInterface(const std::u16string& modulePath, std::u16string name);

    Type* registerExternalType(const std::u16string& modulePath, std::u16string name);
    Type* lookupExternalType(const std::u16string& modulePath, const std::u16string& name) const;

    Type* registerEnum(const std::u16string& modulePath, std::u16string name);
    Type* lookupEnum(const std::u16string& modulePath, const std::u16string& name) const;

    Type* lookupNamedType(const std::u16string& modulePath, const std::u16string& name) const;

    // Generics.
    Type* getTypeParam(const void* owner, int index, std::u16string name,
                       const std::vector<StructInfo*>& bounds);
    Type* instantiate(Type* templateType, const std::vector<Type*>& args);
    Type* substitute(Type* t, const void* owner, const std::vector<Type*>& args);
    // Fill any instantiations whose template was not yet collected when first
    // requested. Returns true if any work was done (loop to a fixpoint).
    bool materializeInstantiations();
    // Fill one deferred instantiation now if its template is ready (used when a
    // class's generic base must be complete before the class is laid out).
    void ensureFilled(StructInfo* inst);
    // Re-copy vtable slots, vtable size, and slot ABI throws-ness from each
    // template onto its instances. Instances filled during class layout copy
    // that state before final vtable assignment runs, so it is refreshed after.
    void refreshInstantiationInheritance();

    // All class/struct instantiations created so far (for monomorphized codegen).
    const std::vector<Type*>& classInstantiations() const { return instantiationList_; }

    // The Type that owns an instantiation's StructInfo.
    Type* typeForInstance(StructInfo* inst) const {
        auto it = instanceTypes_.find(inst);
        return it == instanceTypes_.end() ? nullptr : it->second;
    }

    // True if the type mentions any unsubstituted type parameter (an "open"
    // type, e.g. a generic base recorded on a template).
    static bool containsTypeParam(const Type* t);

    // Generic free-function instantiations, recorded at call sites.
    struct FunctionInstantiation {
        Symbol* function;
        std::vector<Type*> args;
    };
    void recordFunctionInstantiation(Symbol* fn, std::vector<Type*> args);
    const std::vector<FunctionInstantiation>& functionInstantiations() const {
        return functionInstantiations_;
    }

private:
    Type* instantiateInternal(StructInfo* templ, TypeKind kind, const std::vector<Type*>& args);
    void fillInstantiation(StructInfo* inst, StructInfo* templ, const std::vector<Type*>& args);

    struct PendingInstantiation {
        StructInfo* inst;
        StructInfo* templ;
        std::vector<Type*> args;
    };
    std::vector<PendingInstantiation> pendingInstantiations_;

    struct InstantiationKey {
        StructInfo* templ;
        std::vector<Type*> args;
        bool operator==(const InstantiationKey& o) const {
            return templ == o.templ && args == o.args;
        }
    };
    struct InstantiationKeyHash {
        size_t operator()(const InstantiationKey& k) const noexcept {
            size_t h = std::hash<const void*>{}(k.templ);
            for (Type* a : k.args) h = h * 1315423911u ^ std::hash<const void*>{}(a);
            return h;
        }
    };
    struct Key {
        std::u16string modulePath;
        std::u16string name;
        bool operator==(const Key& o) const { return modulePath == o.modulePath && name == o.name; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const noexcept {
            std::hash<std::u16string> h;
            return h(k.modulePath) * 1315423911u ^ h(k.name);
        }
    };

    std::vector<std::unique_ptr<Type>> ownedTypes;
    std::vector<std::unique_ptr<StructInfo>> ownedStructs;
    std::unordered_map<int, Type*> primitiveCache;
    std::unordered_map<Type*, Type*> optionalCache;
    std::unordered_map<Type*, Type*> arrayCache;
    std::unordered_map<Key, Type*, KeyHash> structCache;
    std::unordered_map<Key, Type*, KeyHash> classCache;
    std::unordered_map<Key, Type*, KeyHash> externalCache;
    std::unordered_map<Key, Type*, KeyHash> enumCache;
    std::map<std::pair<const void*, int>, Type*> typeParamCache;
    std::unordered_map<InstantiationKey, Type*, InstantiationKeyHash> instantiationCache;
    std::vector<Type*> instantiationList_;
    std::unordered_map<StructInfo*, Type*> instanceTypes_;
    std::vector<FunctionInstantiation> functionInstantiations_;
    std::vector<std::unique_ptr<Symbol>> ownedSymbols;
    Type* errorType;
    Type* nullType;

    Type* allocate(TypeKind k, Type* inner = nullptr);
};
