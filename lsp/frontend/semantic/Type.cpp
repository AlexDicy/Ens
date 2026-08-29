#include "Type.h"

#include "Symbol.h"

bool sameParameterTypes(const Symbol* a, const Symbol* b) {
    if (!a || !b) return false;
    if (a->paramTypes.size() != b->paramTypes.size()) return false;
    for (size_t i = 0; i < a->paramTypes.size(); ++i) {
        Type* pa = a->paramTypes[i];
        Type* pb = b->paramTypes[i];
        if (!pa || !pb) return false;
        if (!pa->equals(pb)) return false;
    }
    return true;
}

const MethodInfo* declaredToString(const StructInfo* info) {
    if (!info) return nullptr;
    for (const MethodInfo& m : info->methods) {
        if (m.name == u"toString") return &m;
    }
    return nullptr;
}

int StructInfo::findMethodIndexBySignature(const std::u16string& methodName,
                                           const Symbol* like) const {
    for (size_t i = 0; i < methods.size(); ++i) {
        if (methods[i].name != methodName) continue;
        if (sameParameterTypes(methods[i].symbol, like)) return static_cast<int>(i);
    }
    return -1;
}

StructInfo* StructInfo::classDeclaringMethodBySignature(const std::u16string& methodName,
                                                        const Symbol* like) {
    for (StructInfo* s = this; s; s = s->baseInfo) {
        if (s->findMethodIndexBySignature(methodName, like) >= 0) return s;
    }
    return nullptr;
}

int StructInfo::findZeroArgMethodIndex(const std::u16string& methodName) const {
    for (size_t i = 0; i < methods.size(); ++i) {
        if (methods[i].name != methodName) continue;
        if (methods[i].symbol && methods[i].symbol->paramTypes.empty()) return static_cast<int>(i);
    }
    return -1;
}

StructInfo* StructInfo::classDeclaringZeroArgMethod(const std::u16string& methodName) {
    for (StructInfo* s = this; s; s = s->baseInfo) {
        if (s->findZeroArgMethodIndex(methodName) >= 0) return s;
    }
    return nullptr;
}

bool StructInfo::conformsToInterface(const StructInfo* iface) const {
    for (const StructInfo* s = this; s; s = s->baseInfo) {
        if (s == iface) return true;
        for (const Type* t : s->implementedInterfaces) {
            for (const StructInfo* i = t ? t->structInfo : nullptr; i; i = i->baseInfo) {
                if (i == iface) return true;
            }
        }
    }
    return false;
}

bool Type::isInteger() const {
    switch (kind) {
        case TypeKind::Byte:
        case TypeKind::Short:
        case TypeKind::UShort:
        case TypeKind::Int:
        case TypeKind::UInt:
        case TypeKind::Long:
        case TypeKind::ULong:
        case TypeKind::Char:
            return true;
        default:
            return false;
    }
}

bool Type::isFloat() const {
    switch (kind) {
        case TypeKind::Float:
        case TypeKind::Double:
        case TypeKind::Decimal:
            return true;
        default:
            return false;
    }
}

bool Type::isNumeric() const {
    return isInteger() || isFloat();
}

bool Type::isPrimitive() const {
    switch (kind) {
        case TypeKind::Bool:
        case TypeKind::Byte:
        case TypeKind::Short:
        case TypeKind::UShort:
        case TypeKind::Int:
        case TypeKind::UInt:
        case TypeKind::Long:
        case TypeKind::ULong:
        case TypeKind::Float:
        case TypeKind::Double:
        case TypeKind::Char:
            return true;
        case TypeKind::Decimal:
        case TypeKind::String:
        case TypeKind::Void:
        case TypeKind::Null:
        case TypeKind::Optional:
        case TypeKind::Array:
        case TypeKind::Struct:
        case TypeKind::Class:
        case TypeKind::Enum:
        case TypeKind::External:
        case TypeKind::TypeParam:
        case TypeKind::Error:
            return false;
    }
    return false;
}

bool Type::equals(const Type* other) const {
    if (this == other) return true;
    if (!other) return false;
    if (kind != other->kind) return false;
    if (kind == TypeKind::Optional || kind == TypeKind::Array) {
        return inner && other->inner && inner->equals(other->inner);
    }
    if (kind == TypeKind::Struct || kind == TypeKind::Class ||
        kind == TypeKind::Enum || kind == TypeKind::External) {
        return structInfo == other->structInfo;
    }
    if (kind == TypeKind::TypeParam) {
        return paramOwner == other->paramOwner && paramIndex == other->paramIndex;
    }
    return true;
}

bool Type::assignableFrom(const Type* source) const {
    if (!source || source->isError() || isError()) return true;
    if (equals(source)) return true;
    if (source->widensTo(this)) return true;
    // Class upcast: a derived class is assignable to any of its ancestors, and
    // a class converts implicitly to each interface it implements.
    if (isClass() && source->isClass() && structInfo && source->structInfo &&
        source->structInfo->isSubclassOrConforms(structInfo)) {
        return true;
    }
    if (isOptional()) {
        // 'null' fills the outermost level. Anything else has to reach the payload, wrapping one
        // level per step: a 'T?' fills the payload of a 'T??' and a bare 'T' fills the payload of
        // that 'T?' in turn. Nothing lets a deeper value reach a shallower type.
        if (source->isNull()) return true;
        // A nullable class accepts a derived or conforming class (nullable or not).
        if (inner && inner->isClass() && inner->structInfo) {
            const Type* src = source->isOptional() ? source->inner : source;
            if (src && src->isClass() && src->structInfo &&
                src->structInfo->isSubclassOrConforms(inner->structInfo)) {
                return true;
            }
        }
        if (source->isOptional()) {
            return inner && inner->isOptional() && inner->assignableFrom(source);
        }
        return inner && inner->assignableFrom(source);
    }
    return false;
}

bool Type::isSignedInteger() const {
    switch (kind) {
        case TypeKind::Short:
        case TypeKind::Int:
        case TypeKind::Long:
            return true;
        default:
            return false;
    }
}

bool Type::isUnsignedInteger() const {
    switch (kind) {
        case TypeKind::Byte:
        case TypeKind::UShort:
        case TypeKind::UInt:
        case TypeKind::ULong:
        case TypeKind::Char:
            return true;
        default:
            return false;
    }
}

int Type::integerBitWidth() const {
    switch (kind) {
        case TypeKind::Byte:                       return 8;
        case TypeKind::Short:
        case TypeKind::UShort:                     return 16;
        case TypeKind::Int:
        case TypeKind::UInt:
        case TypeKind::Char:                       return 32;
        case TypeKind::Long:
        case TypeKind::ULong:                      return 64;
        default:                                   return 0;
    }
}

int Type::floatBitWidth() const {
    switch (kind) {
        case TypeKind::Float:  return 32;
        case TypeKind::Double: return 64;
        default:               return 0;
    }
}

bool Type::widensTo(const Type* target) const {
    if (!target || target->isError() || isError()) return false;
    if (equals(target)) return false;  // identity covered by equals

    // Integer -> integer.
    if (isInteger() && target->isInteger()) {
        int srcW = integerBitWidth();
        int dstW = target->integerBitWidth();
        if (isSignedInteger() && target->isSignedInteger()) {
            return dstW > srcW;
        }
        if (isUnsignedInteger() && target->isUnsignedInteger()) {
            return dstW > srcW;
        }
        if (isUnsignedInteger() && target->isSignedInteger()) {
            return dstW > srcW;  // strictly wider signed holds all unsigned bits
        }
        // signed -> unsigned: lossless never (negatives don't survive).
        return false;
    }

    // Integer -> float. A `char` is a code point rather than a quantity: arithmetic on it in
    // integers is meaningful, but as a floating-point number it has no use, so it converts
    // only where the program asks for it with `as`.
    if (isInteger() && target->isFloat()) {
        if (kind == TypeKind::Char) return false;
        int srcW = integerBitWidth();
        int mantissa = target->floatBitWidth() == 32 ? 24 : 53;
        return srcW <= mantissa;
    }

    // float -> float (only float -> double in v1).
    if (isFloat() && target->isFloat()) {
        return floatBitWidth() < target->floatBitWidth();
    }

    return false;
}

std::string Type::toString() const {
    switch (kind) {
        case TypeKind::Bool:     return "bool";
        case TypeKind::Byte:     return "byte";
        case TypeKind::Short:    return "short";
        case TypeKind::UShort:   return "ushort";
        case TypeKind::Int:      return "int";
        case TypeKind::UInt:     return "uint";
        case TypeKind::Long:     return "long";
        case TypeKind::ULong:    return "ulong";
        case TypeKind::Float:    return "float";
        case TypeKind::Double:   return "double";
        case TypeKind::Decimal:  return "decimal";
        case TypeKind::Char:     return "char";
        case TypeKind::String:   return "string";
        case TypeKind::Void:     return "void";
        case TypeKind::Null:     return "null";
        case TypeKind::Optional: return (inner ? inner->toString() : std::string("?")) + "?";
        case TypeKind::Array:    return (inner ? inner->toString() : std::string("?")) + "[]";
        case TypeKind::Struct:
        case TypeKind::Class:    {
            std::string r;
            if (structInfo) {
                r.reserve(structInfo->name.size());
                for (char16_t c : structInfo->name) r.push_back(c < 128 ? static_cast<char>(c) : '?');
                if (structInfo->templateOf && !structInfo->typeArgs.empty()) {
                    r.push_back('<');
                    for (size_t i = 0; i < structInfo->typeArgs.size(); ++i) {
                        if (i) r += ", ";
                        r += structInfo->typeArgs[i] ? structInfo->typeArgs[i]->toString() : "?";
                    }
                    r.push_back('>');
                }
            } else {
                r = (kind == TypeKind::Class ? "<class>" : "<struct>");
            }
            return r;
        }
        case TypeKind::Enum: {
            std::string r;
            if (structInfo) {
                r.reserve(structInfo->name.size());
                for (char16_t c : structInfo->name) r.push_back(c < 128 ? static_cast<char>(c) : '?');
            } else {
                r = "<enum>";
            }
            return r;
        }
        case TypeKind::External: {
            std::string r;
            if (structInfo) {
                r.reserve(structInfo->name.size());
                for (char16_t c : structInfo->name) r.push_back(c < 128 ? static_cast<char>(c) : '?');
            } else {
                r = "<external>";
            }
            return r;
        }
        case TypeKind::TypeParam: {
            std::string r;
            r.reserve(paramName.size());
            for (char16_t c : paramName) r.push_back(c < 128 ? static_cast<char>(c) : '?');
            return r.empty() ? "<typeparam>" : r;
        }
        case TypeKind::Error:    return "<error>";
    }
    return "<unknown>";
}
