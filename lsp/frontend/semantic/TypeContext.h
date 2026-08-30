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
    Type* getFunction(std::vector<Type*> params, Type* returnType);
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

    // Every class registered anywhere in the program, interfaces excluded and
    // instantiations included, in registration order. Codegen asks whole-program
    // questions of it, such as which classes a value of an interface type can
    // hold at run time.
    std::vector<StructInfo*> registeredClasses() const;

    // The Type that owns an instantiation's StructInfo.
    Type* typeForInstance(StructInfo* inst) const {
        auto it = instanceTypes_.find(inst);
        return it == instanceTypes_.end() ? nullptr : it->second;
    }

    // The canonical class Type owning `info`, whether it is a plain class or a
    // generic instantiation. Null when `info` is not a registered class.
    Type* classTypeFor(StructInfo* info) const {
        if (!info) return nullptr;
        if (Type* t = typeForInstance(info)) return t;
        return lookupClass(info->modulePath, info->name);
    }

    // True if the type mentions any unsubstituted type parameter (an "open"
    // type, e.g. a generic base recorded on a template).
    static bool containsTypeParam(const Type* t);

    // A generic instantiation chain that never terminates, ready to report at
    // its recursive field. Drained by the analyzer, which clears the list.
    struct InstantiationOverflow {
        StructInfo* templ;
        std::string message;
        int line;
        int column;
        int length;
    };
    std::vector<InstantiationOverflow> takeInstantiationOverflows() {
        return std::move(instantiationOverflows_);
    }

    // Generic free-function instantiations, recorded at call sites.
    struct FunctionInstantiation {
        Symbol* function;
        std::vector<Type*> args;
    };
    void recordFunctionInstantiation(Symbol* fn, std::vector<Type*> args);
    const std::vector<FunctionInstantiation>& functionInstantiations() const {
        return functionInstantiations_;
    }

    // The cap the class-instantiation depth guard uses; codegen applies the same
    // bound to the generic-function instantiation cascade.
    static constexpr int maxInstantiationDepth() { return kMaxInstantiationDepth; }

private:
    Type* instantiateInternal(StructInfo* templ, TypeKind kind, const std::vector<Type*>& args);
    void fillInstantiation(StructInfo* inst, StructInfo* templ, const std::vector<Type*>& args);
    void recordInstantiationOverflow();

    static constexpr int kMaxInstantiationDepth = 200;
    std::vector<Type*> instantiationChain_;
    std::vector<InstantiationOverflow> instantiationOverflows_;

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
    struct FunctionKey {
        std::vector<Type*> params;
        Type* returnType;
        bool operator==(const FunctionKey& o) const {
            return returnType == o.returnType && params == o.params;
        }
    };
    struct FunctionKeyHash {
        size_t operator()(const FunctionKey& k) const noexcept {
            size_t h = std::hash<const void*>{}(k.returnType);
            for (Type* p : k.params) h = h * 1315423911u ^ std::hash<const void*>{}(p);
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
    std::unordered_map<FunctionKey, Type*, FunctionKeyHash> functionCache;
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
