#pragma once
#include <memory>
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

    Type* fromName(const std::u16string& name);

    // Struct registration: returns the new struct Type. Fields can be filled in
    // afterwards via the type's structInfo.
    Type* registerStruct(std::u16string name);
    Type* lookupStruct(const std::u16string& name) const;

private:
    std::vector<std::unique_ptr<Type>> ownedTypes;
    std::vector<std::unique_ptr<StructInfo>> ownedStructs;
    std::unordered_map<int, Type*> primitiveCache;
    std::unordered_map<Type*, Type*> optionalCache;
    std::unordered_map<std::u16string, Type*> structCache;
    Type* errorType;
    Type* nullType;

    Type* allocate(TypeKind k, Type* inner = nullptr);
};
