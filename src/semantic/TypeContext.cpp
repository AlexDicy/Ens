#include "TypeContext.h"

TypeContext::TypeContext() {
    errorType = allocate(TypeKind::Error);
    nullType = allocate(TypeKind::Null);
}

Type* TypeContext::allocate(TypeKind k, Type* inner) {
    auto t = std::make_unique<Type>(k, inner);
    Type* raw = t.get();
    ownedTypes.push_back(std::move(t));
    return raw;
}

Type* TypeContext::getPrimitive(TypeKind k) {
    auto it = primitiveCache.find(static_cast<int>(k));
    if (it != primitiveCache.end()) return it->second;
    Type* t = allocate(k);
    primitiveCache[static_cast<int>(k)] = t;
    return t;
}

Type* TypeContext::getOptional(Type* inner) {
    if (!inner || inner->isError()) return errorType;
    if (inner->isOptional()) return inner;  // T?? collapses to T?
    auto it = optionalCache.find(inner);
    if (it != optionalCache.end()) return it->second;
    Type* t = allocate(TypeKind::Optional, inner);
    optionalCache[inner] = t;
    return t;
}

Type* TypeContext::fromName(const std::u16string& name) {
    if (name == u"bool")    return getPrimitive(TypeKind::Bool);
    if (name == u"byte")    return getPrimitive(TypeKind::Byte);
    if (name == u"short")   return getPrimitive(TypeKind::Short);
    if (name == u"ushort")  return getPrimitive(TypeKind::UShort);
    if (name == u"int")     return getPrimitive(TypeKind::Int);
    if (name == u"uint")    return getPrimitive(TypeKind::UInt);
    if (name == u"long")    return getPrimitive(TypeKind::Long);
    if (name == u"ulong")   return getPrimitive(TypeKind::ULong);
    if (name == u"float")   return getPrimitive(TypeKind::Float);
    if (name == u"double")  return getPrimitive(TypeKind::Double);
    if (name == u"decimal") return getPrimitive(TypeKind::Decimal);
    if (name == u"char")    return getPrimitive(TypeKind::Char);
    if (name == u"string")  return getPrimitive(TypeKind::String);
    if (name == u"void")    return getPrimitive(TypeKind::Void);
    return nullptr;
}
