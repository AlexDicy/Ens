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
        case TypeKind::Struct:
        case TypeKind::Class:
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
    if (kind == TypeKind::Optional) {
        return inner && other->inner && inner->equals(other->inner);
    }
    if (kind == TypeKind::Struct || kind == TypeKind::Class ||
        kind == TypeKind::External) {
        return structInfo == other->structInfo;
    }
    return true;
}

bool Type::assignableFrom(const Type* source) const {
    if (!source || source->isError() || isError()) return true;
    if (equals(source)) return true;
    if (isOptional()) {
        if (source->isNull()) return true;
        if (inner && inner->equals(source)) return true;
        if (source->isOptional() && inner && source->inner && inner->equals(source->inner)) return true;
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
