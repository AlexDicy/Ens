#pragma once
#include <string>
#include <vector>

enum class TypeKind {
    Bool, Byte, Short, UShort, Int, UInt, Long, ULong,
    Float, Double, Decimal, Char, String, Void,
    Null,        // type of the `null` literal — assignable to any Optional
    Optional,    // wraps another type
    Struct,      // user-defined struct
    Error        // sentinel — used to suppress cascading errors
};

class Type;

struct FieldInfo {
    std::u16string name;
    Type* type;
    int line = 0;
    int column = 0;
};

struct StructInfo {
    std::u16string name;
    std::vector<FieldInfo> fields;
    int line = 0;
    int column = 0;

    int findFieldIndex(const std::u16string& fieldName) const {
        for (size_t i = 0; i < fields.size(); ++i) {
            if (fields[i].name == fieldName) return static_cast<int>(i);
        }
        return -1;
    }
};

class Type {
public:
    TypeKind kind;
    Type* inner = nullptr;
    StructInfo* structInfo = nullptr;

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
    bool isStruct() const { return kind == TypeKind::Struct; }

    bool equals(const Type* other) const;
    bool assignableFrom(const Type* source) const;

    std::string toString() const;
};
