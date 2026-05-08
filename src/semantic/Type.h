#pragma once
#include <string>

enum class TypeKind {
    Bool, Byte, Short, UShort, Int, UInt, Long, ULong,
    Float, Double, Decimal, Char, String, Void,
    Null,        // type of the `null` literal — assignable to any Optional
    Optional,    // wraps another type
    Error        // sentinel — used to suppress cascading errors
};

class Type {
public:
    TypeKind kind;
    Type* inner = nullptr;

    explicit Type(TypeKind k) : kind(k) {}
    Type(TypeKind k, Type* i) : kind(k), inner(i) {}

    bool isInteger() const;
    bool isFloat() const;
    bool isNumeric() const;
    bool isOptional() const { return kind == TypeKind::Optional; }
    bool isError() const { return kind == TypeKind::Error; }
    bool isVoid() const { return kind == TypeKind::Void; }
    bool isBool() const { return kind == TypeKind::Bool; }
    bool isNull() const { return kind == TypeKind::Null; }

    bool equals(const Type* other) const;
    bool assignableFrom(const Type* source) const;

    std::string toString() const;
};
