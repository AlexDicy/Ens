#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "Type.h"

class TypeContext {
public:
    TypeContext();

    Type* getPrimitive(TypeKind k);
    Type* getOptional(Type* inner);
    Type* getError() { return errorType; }
    Type* getNull() { return nullType; }

    Type* primitiveFromName(const std::u16string& name);

    Type* registerStruct(const std::u16string& modulePath, std::u16string name);
    Type* lookupStruct(const std::u16string& modulePath, const std::u16string& name) const;

    Type* registerClass(const std::u16string& modulePath, std::u16string name);
    Type* lookupClass(const std::u16string& modulePath, const std::u16string& name) const;

    Type* lookupNamedType(const std::u16string& modulePath, const std::u16string& name) const;

private:
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
    std::unordered_map<Key, Type*, KeyHash> structCache;
    std::unordered_map<Key, Type*, KeyHash> classCache;
    Type* errorType;
    Type* nullType;

    Type* allocate(TypeKind k, Type* inner = nullptr);
};
