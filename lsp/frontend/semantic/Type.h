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
    Function,    // (int, int) -> int: the type of a function value
    Struct,      // user-defined struct (value semantics)
    Class,       // user-defined class (reference semantics, heap-allocated)
    Enum,        // user-defined enum (integer-backed value type)
    External,    // opaque foreign type declared via `external type Name;`
    TypeParam,   // a generic type-parameter placeholder before instantiation
    Error        // sentinel - used to suppress cascading errors
};

class Type;
class Symbol;
struct StructInfo;

// The visibility ladder: private (declaring file, or declaring type for members),
// public (every module in the declaring package), export (packages that consume
// the declaring package). Protected sits alongside: declaring file plus subclasses.
enum class Visibility { Private, Protected, Public, Export };

// The ladder position used by the boundary and leakage checks. Protected shares
// the private floor: its guaranteed reach is the declaring file.
inline int visibilityTier(Visibility v) {
    switch (v) {
        case Visibility::Private:
        case Visibility::Protected: return 0;
        case Visibility::Public:    return 1;
        case Visibility::Export:    return 2;
    }
    return 0;
}

// What a StructInfo was declared as. Classes, interfaces, structs, enums and
// external types all carry a StructInfo, so diagnostics need the real keyword.
enum class DeclKind { Class, Struct, Interface, Enum, External, Primitive };

inline const char* declKindWord(DeclKind k) {
    switch (k) {
        case DeclKind::Struct:    return "struct";
        case DeclKind::Interface: return "interface";
        case DeclKind::Enum:      return "enum";
        case DeclKind::External:  return "external type";
        case DeclKind::Primitive: return "primitive";
        case DeclKind::Class:     return "class";
    }
    return "class";
}

struct FieldInfo {
    std::u16string name;
    Type* type;
    Visibility visibility = Visibility::Private;
    bool isWeak = false;
    int line = 0;
    int column = 0;
    const GreenElement* declaration = nullptr;
    StructInfo* definingClass = nullptr;  // class/struct that declares this field (for visibility)
};

struct EnumMemberInfo {
    std::u16string name;
    int64_t value = 0;
};

struct MethodInfo {
    std::u16string name;
    Symbol* symbol = nullptr;       // function symbol with paramTypes/returnType (no `this`)
    void* declaration = nullptr;    // FuncDecl* (kept void* to avoid AST include cycle)
    Visibility visibility = Visibility::Private;
    bool isConstructor = false;
    bool isDestructor = false;
    bool isOverride = false;
    bool isFinal = false;
    bool isAbstract = false;
    bool isNoreturn = false;
    int vtableSlot = -1;            // >= 0 => dispatched virtually through the vtable
    int itableSlot = -1;            // interface methods: index in the interface's method table
    StructInfo* definingClass = nullptr;  // class that declares this method (for codegen mangling)
};

struct StructInfo {
    std::u16string name;
    std::u16string modulePath;       // owning module's canonical path; "" for single-file/stdin
    std::u16string packagePrefix;    // owning package's prefix; "" for the root package
    DeclKind declKind = DeclKind::Class;
    Visibility visibility = Visibility::Private;
    std::vector<FieldInfo> fields;   // for a class, base fields are flattened in first
    std::vector<MethodInfo> methods; // methods are NOT flattened; walk baseInfo to inherit
    std::vector<EnumMemberInfo> enumMembers;  // for an enum: members in declaration order
    bool enumIsNumeric = false;               // enum with explicit assigned values: convertible to/from integers
    int line = 0;
    int column = 0;

    // Inheritance (classes only; null for structs / no base).
    StructInfo* baseInfo = nullptr;
    int baseFieldCount = 0;          // count of leading `fields` inherited from baseInfo
    int vtableSize = 0;
    uint32_t typeId = 0;             // unique per class, for RTTI
    bool isAbstract = false;
    bool isFinal = false;
    bool isSealed = false;           // direct subclasses restricted to the declaring module
    std::vector<StructInfo*> directSubclasses;  // classes extending this one directly

    // Interfaces. An interface reuses the class type kind (reference semantics)
    // with this flag set; its `methods` are bodiless signatures with itable slots.
    // On a class, `implementedInterfaces` lists the interface types named in its
    // own `implements` clause (base classes contribute theirs via the chain).
    bool isInterface = false;
    std::vector<Type*> implementedInterfaces;

    // Generics. A template carries its type-parameter names and optional bounds;
    // an instantiation points back at its template and records the concrete args.
    bool isTemplate = false;
    bool membersCollected = false;              // template: fields/methods resolved
    std::vector<std::u16string> typeParamNames;
    std::vector<std::vector<StructInfo*>> typeParamBounds;  // parallel to names; empty = unbounded
    StructInfo* templateOf = nullptr;           // instantiation -> its template
    std::vector<Type*> typeArgs;                // instantiation -> concrete type args

    // Searched most derived first: the flattened layout holds inherited fields before own ones,
    // and a subclass field may reuse the name of a private base field, which is not inherited.
    int findFieldIndex(const std::u16string& fieldName) const {
        for (size_t i = fields.size(); i > 0; --i) {
            if (fields[i - 1].name == fieldName) return static_cast<int>(i - 1);
        }
        return -1;
    }
    int findMethodIndex(const std::u16string& methodName) const {
        for (size_t i = 0; i < methods.size(); ++i) {
            if (methods[i].name == methodName) return static_cast<int>(i);
        }
        return -1;
    }
    bool hasOwnConstructor() const {
        for (const auto& m : methods) {
            if (m.isConstructor) return true;
        }
        return false;
    }
    int findConstructorIndex() const {
        for (size_t i = 0; i < methods.size(); ++i) {
            if (methods[i].isConstructor) return static_cast<int>(i);
        }
        return -1;
    }
    int findDestructorIndex() const {
        for (size_t i = 0; i < methods.size(); ++i) {
            if (methods[i].isDestructor) return static_cast<int>(i);
        }
        return -1;
    }
    // Overload-aware lookups (defined in Type.cpp; they need Symbol's parameter list).
    int findMethodIndexBySignature(const std::u16string& methodName, const Symbol* like) const;
    StructInfo* classDeclaringMethodBySignature(const std::u16string& methodName, const Symbol* like);
    int findZeroArgMethodIndex(const std::u16string& methodName) const;
    StructInfo* classDeclaringZeroArgMethod(const std::u16string& methodName);
    // True if this class is `other` or descends from it.
    bool isSubclassOf(const StructInfo* other) const {
        for (const StructInfo* s = this; s; s = s->baseInfo) {
            if (s == other) return true;
        }
        return false;
    }
    // True if this class (or a base class) implements `iface`, or is `iface` itself.
    bool conformsToInterface(const StructInfo* iface) const;
    // Combined subtyping test: subclass for a class target, conformance for an
    // interface target. The single primitive behind implicit reference conversions.
    bool isSubclassOrConforms(const StructInfo* other) const {
        if (!other) return false;
        return other->isInterface ? conformsToInterface(other) : isSubclassOf(other);
    }
    // Nearest class in the chain (self first) that declares `methodName`, or null.
    StructInfo* classDeclaringMethod(const std::u16string& methodName) {
        for (StructInfo* s = this; s; s = s->baseInfo) {
            if (s->findMethodIndex(methodName) >= 0) return s;
        }
        return nullptr;
    }
};

// True when the two function symbols take exactly the same parameter types.
bool sameParameterTypes(const Symbol* a, const Symbol* b);

// A struct's own `toString`, or null when it declares none. Both an explicit
// `.toString()` and an interpolation hole ask this one question to decide whether a
// struct has a text form of its own instead of its JSON form. Its declaration is
// held to `toString() -> string` without `throws`, so it always stands in.
const MethodInfo* declaredToString(const StructInfo* info);

class Type {
public:
    TypeKind kind;
    Type* inner = nullptr;
    StructInfo* structInfo = nullptr;

    // Type-parameter placeholder (kind == TypeParam): identity is (paramOwner,
    // paramIndex); `structInfo` holds the primary bound (the class bound when one
    // exists) and `paramBounds` all bounds; paramName is for display.
    const void* paramOwner = nullptr;
    int paramIndex = -1;
    std::u16string paramName;
    std::vector<StructInfo*> paramBounds;

    // Function type (kind == Function): its parameter types in order and the type it
    // returns. Two of them are the same type only when every part matches, so
    // assignability between them is identity. `hasThrowsClause` records that some
    // reference to this shape carried a `throws` clause; the server does not track which
    // types, so it treats a call through such a value as possibly throwing.
    std::vector<Type*> functionParams;
    Type* functionReturn = nullptr;
    bool hasThrowsClause = false;

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
    bool isFunction() const { return kind == TypeKind::Function; }
    bool isStruct() const { return kind == TypeKind::Struct; }
    bool isClass() const  { return kind == TypeKind::Class; }
    // Interfaces share the class type kind; this narrows to them.
    bool isInterface() const { return kind == TypeKind::Class && structInfo && structInfo->isInterface; }
    bool isEnum() const   { return kind == TypeKind::Enum; }
    bool isExternal() const { return kind == TypeKind::External; }
    bool isTypeParam() const { return kind == TypeKind::TypeParam; }
    bool isString() const { return kind == TypeKind::String; }
    bool hasRecordLayout() const { return isStruct() || isClass(); }

    bool equals(const Type* other) const;
    bool assignableFrom(const Type* source) const;
    // True when this is `declared` instantiated at its own type parameters, which is the type
    // a generic body spells while `this` inside it carries the bare declared type.
    bool isSelfInstantiationOf(const Type* declared) const;
    bool widensTo(const Type* target) const;
    bool isSignedInteger() const;
    bool isUnsignedInteger() const;
    int  integerBitWidth() const;
    int  floatBitWidth() const;

    std::string toString() const;
};
