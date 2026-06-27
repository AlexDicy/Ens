#include "Type.h"

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
    return true;
}

bool Type::assignableFrom(const Type* source) const {
    if (!source || source->isError() || isError()) return true;
    if (equals(source)) return true;
    if (source->widensTo(this)) return true;
    // Class upcast: a derived class is assignable to any of its ancestors.
    if (isClass() && source->isClass() && structInfo && source->structInfo &&
        source->structInfo->isSubclassOf(structInfo)) {
        return true;
    }
    if (isOptional()) {
        if (source->isNull()) return true;
        if (inner && inner->equals(source)) return true;
        if (inner && source->widensTo(inner)) return true;
        if (source->isOptional() && inner && source->inner && inner->equals(source->inner)) return true;
        // A nullable class accepts a derived class (or a nullable derived class).
        if (inner && inner->isClass() && inner->structInfo) {
            const Type* src = source->isOptional() ? source->inner : source;
            if (src && src->isClass() && src->structInfo &&
                src->structInfo->isSubclassOf(inner->structInfo)) {
                return true;
            }
        }
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

    // Integer -> float.
    if (isInteger() && target->isFloat()) {
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
        case TypeKind::Error:    return "<error>";
    }
    return "<unknown>";
}
