#pragma once
#include <optional>
#include <vector>
#include "../SyntaxNode.h"
#include "Common.h"
#include "Expression.h"
#include "Statement.h"
#include "TypeReference.h"

namespace cst::ast {

enum class Visibility { Public, Private, Protected };

class VisibilityModifier;
class ReturnType;
class DefaultValue;
class Parameter;
class ParameterList;
class FuncDecl;
class FieldDecl;
class StructDecl;
class ClassDecl;
class MemberList;
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

class ReturnType {
public:
    SyntaxNode node;
    static std::optional<ReturnType> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::ReturnType) return std::nullopt;
        return ReturnType{n};
    }
    std::optional<TypeReference> typeReference() const;
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
    std::optional<ParameterList> parameterList() const;
    std::vector<Parameter> parameters() const;
    std::optional<ReturnType> returnType() const;
    std::optional<Block> body() const;
    bool isShorthand() const;
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
    std::optional<TypeReference> typeReference() const;
    std::optional<SyntaxNode> nameToken() const;
    std::optional<std::u16string> nameText() const;
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
    std::optional<MemberList> memberList() const;
    std::vector<FieldDecl> fields() const;
    std::vector<FuncDecl> methods() const;
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
    std::optional<MemberList> memberList() const;
    std::vector<FieldDecl> fields() const;
    std::vector<FuncDecl> methods() const;
};

class SourceFile {
public:
    SyntaxNode node;
    static std::optional<SourceFile> cast(const SyntaxNode& n) {
        if (n.kind() != SyntaxKind::SourceFile) return std::nullopt;
        return SourceFile{n};
    }
    std::vector<FuncDecl> functions() const;
    std::vector<StructDecl> structs() const;
    std::vector<ClassDecl> classes() const;
    std::vector<TypedVarDeclStatement> topLevelVarDecls() const;
};

}  // namespace cst::ast
