#pragma once
#include <cstdint>
#include <string>
#include <vector>

class GreenElement;

enum class TypeKind {
    Bool, Byte, Short, UShort, Int, UInt, Long, ULong,
    Float, Double, Decimal, Char, String, Void,
    Null,        // type of the `null` literal - assignable to any Optional
    Optional,    // wraps another type
    Array,       // T[]
    Struct,      // user-defined struct (value semantics)
    Class,       // user-defined class (reference semantics, heap-allocated)
    External,    // opaque foreign type declared via `external type Name;`
    Error        // sentinel - used to suppress cascading errors
};

class Type;
class Symbol;
struct StructInfo;

enum class Visibility { Public, Private, Protected };

struct FieldInfo {
    std::u16string name;
    Type* type;
    Visibility visibility = Visibility::Public;
    bool isWeak = false;
    int line = 0;
    int column = 0;
    const GreenElement* declaration = nullptr;
    StructInfo* definingClass = nullptr;  // class/struct that declares this field (for visibility)
};

struct MethodInfo {
    std::u16string name;
    Symbol* symbol = nullptr;       // function symbol with paramTypes/returnType (no `this`)
    void* declaration = nullptr;    // FuncDecl* (kept void* to avoid AST include cycle)
    Visibility visibility = Visibility::Public;
    bool isOverride = false;
    bool isFinal = false;
    bool isAbstract = false;
    int vtableSlot = -1;            // >= 0 => dispatched virtually through the vtable
    StructInfo* definingClass = nullptr;  // class that declares this method (for codegen mangling)
};

struct StructInfo {
    std::u16string name;
    std::u16string modulePath;       // owning module's canonical path; "" for single-file/stdin
    Visibility visibility = Visibility::Public;  // only public types are reachable cross-module
    std::vector<FieldInfo> fields;   // for a class, base fields are flattened in first
    std::vector<MethodInfo> methods; // methods are NOT flattened; walk baseInfo to inherit
    int line = 0;
    int column = 0;

    // Inheritance (classes only; null for structs / no base).
    StructInfo* baseInfo = nullptr;
    int baseFieldCount = 0;          // count of leading `fields` inherited from baseInfo
    int vtableSize = 0;
    uint32_t typeId = 0;             // unique per class, for RTTI
    bool isAbstract = false;
    bool isFinal = false;

    int findFieldIndex(const std::u16string& fieldName) const {
        for (size_t i = 0; i < fields.size(); ++i) {
            if (fields[i].name == fieldName) return static_cast<int>(i);
        }
        return -1;
    }
    int findMethodIndex(const std::u16string& methodName) const {
        for (size_t i = 0; i < methods.size(); ++i) {
            if (methods[i].name == methodName) return static_cast<int>(i);
        }
        return -1;
    }
    // True if this class is `other` or descends from it.
    bool isSubclassOf(const StructInfo* other) const {
        for (const StructInfo* s = this; s; s = s->baseInfo) {
            if (s == other) return true;
        }
        return false;
    }
    // Nearest class in the chain (self first) that declares `methodName`, or null.
    StructInfo* classDeclaringMethod(const std::u16string& methodName) {
        for (StructInfo* s = this; s; s = s->baseInfo) {
            if (s->findMethodIndex(methodName) >= 0) return s;
        }
        return nullptr;
    }
    // Topmost class in the chain (deepest ancestor) that declares `methodName`, or null.
    StructInfo* rootClassDeclaringMethod(const std::u16string& methodName) {
        StructInfo* found = nullptr;
        for (StructInfo* s = this; s; s = s->baseInfo) {
            if (s->findMethodIndex(methodName) >= 0) found = s;
        }
        return found;
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
    bool isPrimitive() const;
    bool isOptional() const { return kind == TypeKind::Optional; }
    bool isError() const { return kind == TypeKind::Error; }
    bool isVoid() const { return kind == TypeKind::Void; }
    bool isBool() const { return kind == TypeKind::Bool; }
    bool isNull() const { return kind == TypeKind::Null; }
    bool isArray() const { return kind == TypeKind::Array; }
    bool isStruct() const { return kind == TypeKind::Struct; }
    bool isClass() const  { return kind == TypeKind::Class; }
    bool isExternal() const { return kind == TypeKind::External; }
    bool isString() const { return kind == TypeKind::String; }
    bool hasRecordLayout() const { return isStruct() || isClass(); }

    bool equals(const Type* other) const;
    bool assignableFrom(const Type* source) const;
    bool widensTo(const Type* target) const;
    bool isSignedInteger() const;
    bool isUnsignedInteger() const;
    int  integerBitWidth() const;
    int  floatBitWidth() const;

    std::string toString() const;
};
