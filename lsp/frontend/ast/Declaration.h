#pragma once
#include <optional>
#include <vector>
#include "../cst/SyntaxNode.h"
#include "Common.h"
#include "Expression.h"
#include "Statement.h"
#include "TypeReference.h"

namespace ast {

enum class Visibility { Public, Private, Protected, Export };

class VisibilityModifier;
class TypeParam;
class TypeParamList;
class ReturnType;
class ThrowsClause;
class CatchClause;
class DefaultValue;
class Parameter;
class ParameterList;
class FuncDecl;
class FieldDecl;
class StructDecl;
class ClassDecl;
class InterfaceDecl;
class PrimitiveDecl;
class ImplementsClause;
class MemberList;
class ImportDecl;
class ImportPath;
class ExternalTypeDecl;
class ExternalFuncDecl;
class ExternalBlock;
class EnumDecl;
class EnumMember;
class TestDecl;
class SourceFile;

class VisibilityModifier {
public:
    SyntaxNode node;
    static std::optional<VisibilityModifier> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::VisibilityModifier) return std::nullopt;
        return VisibilityModifier{n};
    }
    Visibility visibility() const;
};

class TypeParam {
public:
    SyntaxNode node;
    static std::optional<TypeParam> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::TypeParam) return std::nullopt;
        return TypeParam{n};
    }
    std::optional<SyntaxNode> nameToken() const;
    std::optional<std::u16string> nameText() const;
    std::optional<TypeReference> bound() const;
    // All bounds in source order: `T: Base + Comparable` yields both.
    std::vector<TypeReference> bounds() const;
};

class TypeParamList {
public:
    SyntaxNode node;
    static std::optional<TypeParamList> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::TypeParamList) return std::nullopt;
        return TypeParamList{n};
    }
    std::vector<TypeParam> params() const;
};

class ReturnType {
public:
    SyntaxNode node;
    static std::optional<ReturnType> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ReturnType) return std::nullopt;
        return ReturnType{n};
    }
    std::optional<TypeReference> typeReference() const;
};

class ThrowsClause {
public:
    SyntaxNode node;
    static std::optional<ThrowsClause> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ThrowsClause) return std::nullopt;
        return ThrowsClause{n};
    }
    // declared exception types; empty for an inferred `throws`.
    std::vector<TypeReference> types() const;
};

class CatchClause {
public:
    SyntaxNode node;
    static std::optional<CatchClause> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::CatchClause) return std::nullopt;
        return CatchClause{n};
    }
    std::optional<TypeReference> typeReference() const;
    std::optional<SyntaxNode> nameToken() const;
    std::optional<std::u16string> nameText() const;
    std::optional<Block> body() const;
};

class DefaultValue {
public:
    SyntaxNode node;
    static std::optional<DefaultValue> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::DefaultValue) return std::nullopt;
        return DefaultValue{n};
    }
    std::optional<Expression> expression() const;
};

class Parameter {
public:
    SyntaxNode node;
    static std::optional<Parameter> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::Parameter) return std::nullopt;
        return Parameter{n};
    }
    bool isThisField() const;
    bool isOut() const;
    std::optional<SyntaxNode> nameToken() const;
    std::optional<std::u16string> nameText() const;
    std::optional<TypeReference> typeReference() const;
    std::optional<DefaultValue> defaultValue() const;
};

class ParameterList {
public:
    SyntaxNode node;
    static std::optional<ParameterList> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ParamList) return std::nullopt;
        return ParameterList{n};
    }
    std::vector<Parameter> parameters() const;
};

class FuncDecl {
public:
    SyntaxNode node;
    static std::optional<FuncDecl> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::FuncDecl) return std::nullopt;
        return FuncDecl{n};
    }
    std::optional<VisibilityModifier> visibilityModifier() const;
    Visibility visibility() const;
    std::optional<SyntaxNode> nameToken() const;
    std::optional<std::u16string> nameText() const;
    std::optional<TypeParamList> typeParamList() const;
    std::vector<TypeParam> typeParams() const;
    std::optional<ParameterList> parameterList() const;
    std::vector<Parameter> parameters() const;
    std::optional<ReturnType> returnType() const;
    std::optional<Block> body() const;
    bool isShorthand() const;
    bool isConstructor() const;
    bool isDestructor() const;
    bool isOverride() const;
    bool isFinal() const;
    bool isAbstract() const;
    bool isNoreturn() const;
    bool isThrows() const;
    std::optional<ThrowsClause> throwsClause() const;
    std::optional<SyntaxNode> throwsToken() const;
    std::vector<TypeReference> declaredThrowsTypes() const;
    std::vector<CatchClause> catchClauses() const;
};

class FieldDecl {
public:
    SyntaxNode node;
    static std::optional<FieldDecl> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::FieldDecl) return std::nullopt;
        return FieldDecl{n};
    }
    std::optional<VisibilityModifier> visibilityModifier() const;
    Visibility visibility() const;
    bool isWeak() const;
    std::optional<TypeReference> typeReference() const;
    std::optional<SyntaxNode> nameToken() const;
    std::optional<std::u16string> nameText() const;
    std::optional<DefaultValue> defaultValue() const;
};

class MemberList {
public:
    SyntaxNode node;
    static std::optional<MemberList> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::MemberList) return std::nullopt;
        return MemberList{n};
    }
    std::vector<FieldDecl> fields() const;
    std::vector<FuncDecl> methods() const;
};

class StructDecl {
public:
    SyntaxNode node;
    static std::optional<StructDecl> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::StructDecl) return std::nullopt;
        return StructDecl{n};
    }
    std::optional<VisibilityModifier> visibilityModifier() const;
    Visibility visibility() const;
    std::optional<SyntaxNode> nameToken() const;
    std::optional<std::u16string> nameText() const;
    std::optional<TypeParamList> typeParamList() const;
    std::vector<TypeParam> typeParams() const;
    std::optional<MemberList> memberList() const;
    std::vector<FieldDecl> fields() const;
    std::vector<FuncDecl> methods() const;
};

class ImplementsClause {
public:
    SyntaxNode node;
    static std::optional<ImplementsClause> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ImplementsClause) return std::nullopt;
        return ImplementsClause{n};
    }
    std::vector<TypeReference> types() const;
};

class ClassDecl {
public:
    SyntaxNode node;
    static std::optional<ClassDecl> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ClassDecl) return std::nullopt;
        return ClassDecl{n};
    }
    std::optional<VisibilityModifier> visibilityModifier() const;
    Visibility visibility() const;
    std::optional<SyntaxNode> nameToken() const;
    std::optional<std::u16string> nameText() const;
    std::optional<TypeParamList> typeParamList() const;
    std::vector<TypeParam> typeParams() const;
    std::optional<SyntaxNode> baseClassToken() const;
    std::optional<std::u16string> baseClassName() const;
    std::vector<TypeReference> baseTypeArguments() const;
    std::optional<ImplementsClause> implementsClause() const;
    std::vector<TypeReference> implementedInterfaceRefs() const;
    bool isAbstract() const;
    bool isFinal() const;
    bool isSealed() const;
    std::optional<MemberList> memberList() const;
    std::vector<FieldDecl> fields() const;
    std::vector<FuncDecl> methods() const;
};

class InterfaceDecl {
public:
    SyntaxNode node;
    static std::optional<InterfaceDecl> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::InterfaceDecl) return std::nullopt;
        return InterfaceDecl{n};
    }
    std::optional<VisibilityModifier> visibilityModifier() const;
    Visibility visibility() const;
    std::optional<SyntaxNode> nameToken() const;
    std::optional<std::u16string> nameText() const;
    std::optional<TypeParamList> typeParamList() const;
    std::vector<TypeParam> typeParams() const;
    std::optional<SyntaxNode> baseInterfaceToken() const;
    std::optional<std::u16string> baseInterfaceName() const;
    std::vector<TypeReference> baseTypeArguments() const;
    std::optional<MemberList> memberList() const;
    std::vector<FieldDecl> fields() const;
    std::vector<FuncDecl> methods() const;
};

// `primitive string implements Comparable<string> { ... }`: what the standard library declares a
// primitive's members to be. The name position holds the primitive's own keyword.
class PrimitiveDecl {
public:
    SyntaxNode node;
    static std::optional<PrimitiveDecl> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::PrimitiveDecl) return std::nullopt;
        return PrimitiveDecl{n};
    }
    std::optional<SyntaxNode> nameToken() const;
    std::optional<std::u16string> nameText() const;
    std::vector<TypeReference> implementedInterfaces() const;
    std::optional<MemberList> memberList() const;
    std::vector<FieldDecl> fields() const;
    std::vector<FuncDecl> methods() const;
};

class ImportPath {
public:
    SyntaxNode node;
    static std::optional<ImportPath> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ImportPath) return std::nullopt;
        return ImportPath{n};
    }
    bool isPackage() const;
    std::vector<SyntaxNode> segmentTokens() const;
    std::vector<std::u16string> segments() const;
};

class ImportDecl {
public:
    SyntaxNode node;
    static std::optional<ImportDecl> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ImportDecl) return std::nullopt;
        return ImportDecl{n};
    }
    std::optional<ImportPath> importPath() const;
    bool isPackage() const;
    std::optional<SyntaxNode> aliasToken() const;
    std::optional<std::u16string> aliasText() const;
    std::vector<std::u16string> pathSegments() const;
    std::optional<std::u16string> namespaceName() const;
    std::u16string modulePath() const;
};

class ExternalTypeDecl {
public:
    SyntaxNode node;
    static std::optional<ExternalTypeDecl> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ExternalTypeDecl) return std::nullopt;
        return ExternalTypeDecl{n};
    }
    std::optional<VisibilityModifier> visibilityModifier() const;
    Visibility visibility() const;
    std::optional<SyntaxNode> nameToken() const;
    std::optional<std::u16string> nameText() const;
};

class ExternalFuncDecl {
public:
    SyntaxNode node;
    static std::optional<ExternalFuncDecl> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ExternalFuncDecl) return std::nullopt;
        return ExternalFuncDecl{n};
    }
    std::optional<SyntaxNode> nameToken() const;
    std::optional<std::u16string> nameText() const;
    std::optional<ParameterList> parameterList() const;
    std::vector<Parameter> parameters() const;
    std::optional<ReturnType> returnType() const;
};

class ExternalBlock {
public:
    SyntaxNode node;
    static std::optional<ExternalBlock> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ExternalBlock) return std::nullopt;
        return ExternalBlock{n};
    }
    std::optional<VisibilityModifier> visibilityModifier() const;
    Visibility visibility() const;
    std::optional<std::u16string> libraryName() const;
    std::vector<ExternalFuncDecl> declarations() const;
};

class EnumMember {
public:
    SyntaxNode node;
    static std::optional<EnumMember> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::EnumMember) return std::nullopt;
        return EnumMember{n};
    }
    std::optional<SyntaxNode> nameToken() const;
    std::optional<std::u16string> nameText() const;
    std::optional<DefaultValue> value() const;
};

class EnumDecl {
public:
    SyntaxNode node;
    static std::optional<EnumDecl> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::EnumDecl) return std::nullopt;
        return EnumDecl{n};
    }
    std::optional<VisibilityModifier> visibilityModifier() const;
    Visibility visibility() const;
    std::optional<SyntaxNode> nameToken() const;
    std::optional<std::u16string> nameText() const;
    std::vector<EnumMember> members() const;
};

class TestDecl {
public:
    SyntaxNode node;
    static std::optional<TestDecl> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::TestDecl) return std::nullopt;
        return TestDecl{n};
    }
    std::optional<SyntaxNode> descriptionToken() const;
    // The description with quotes stripped and escape sequences decoded.
    std::optional<std::u16string> descriptionText() const;
    // The description literal exactly as written, including the quotes.
    std::optional<std::u16string> rawDescriptionLiteral() const;
    std::optional<Block> body() const;
};

class SourceFile {
public:
    SyntaxNode node;
    static std::optional<SourceFile> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::SourceFile) return std::nullopt;
        return SourceFile{n};
    }
    std::vector<ImportDecl> imports() const;
    std::vector<FuncDecl> functions() const;
    std::vector<StructDecl> structs() const;
    std::vector<ClassDecl> classes() const;
    std::vector<InterfaceDecl> interfaces() const;
    std::vector<PrimitiveDecl> primitiveBindings() const;
    std::vector<EnumDecl> enums() const;
    std::vector<TestDecl> tests() const;
    std::vector<TypedVarDeclStatement> topLevelVarDecls() const;
    std::vector<ExternalTypeDecl> externalTypes() const;
    std::vector<ExternalBlock> externalBlocks() const;
};

}  // namespace ast
