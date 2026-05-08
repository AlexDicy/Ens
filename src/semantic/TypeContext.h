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

private:
    std::vector<std::unique_ptr<Type>> ownedTypes;
    std::unordered_map<int, Type*> primitiveCache;
    std::unordered_map<Type*, Type*> optionalCache;
    Type* errorType;
    Type* nullType;

    Type* allocate(TypeKind k, Type* inner = nullptr);
};
