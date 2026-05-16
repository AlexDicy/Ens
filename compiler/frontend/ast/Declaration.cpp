#include "Declaration.h"

namespace ast {

// === VisibilityModifier ===

Visibility VisibilityModifier::visibility() const {
    for (auto& c : node.children()) {
        if (isTrivia(c.kind()) || !c.isToken()) continue;
        switch (c.kind()) {
            case SyntaxKind::KwPrivate:   return Visibility::Private;
            case SyntaxKind::KwProtected: return Visibility::Protected;
            case SyntaxKind::KwPublic:    return Visibility::Public;
            default: break;
        }
    }
    return Visibility::Public;
}

static Visibility visibilityOfDecl(const SyntaxNode& decl) {
    for (auto& c : decl.children()) {
        if (c.kind() == SyntaxKind::VisibilityModifier) {
            if (auto v = VisibilityModifier::cast(c)) return v->visibility();
        }
    }
    return Visibility::Public;
}

static std::optional<VisibilityModifier> visibilityOfDeclNode(const SyntaxNode& decl) {
    if (auto v = firstChildNode(decl, SyntaxKind::VisibilityModifier)) {
        return VisibilityModifier::cast(*v);
    }
    return std::nullopt;
}

static std::optional<SyntaxNode> firstIdentAfterKeyword(const SyntaxNode& decl, SyntaxKind keyword) {
    bool seenKw = false;
    for (auto& c : decl.children()) {
        if (isTrivia(c.kind())) continue;
        if (c.kind() == keyword) { seenKw = true; continue; }
        if (seenKw && c.kind() == SyntaxKind::Identifier) return c;
    }
    return std::nullopt;
}

// === ReturnType ===

std::optional<TypeReference> ReturnType::typeReference() const {
    if (auto tr = firstChildNode(node, SyntaxKind::TypeRef)) return TypeReference::cast(*tr);
    return std::nullopt;
}

// === DefaultValue ===

std::optional<Expression> DefaultValue::expression() const {
    for (auto& c : node.children()) {
        if (auto e = Expression::cast(c)) return e;
    }
    return std::nullopt;
}

// === Parameter ===

bool Parameter::isThisField() const {
    for (auto& c : node.children()) {
        if (isTrivia(c.kind())) continue;
        if (c.kind() == SyntaxKind::KwThis) return true;
        break;
    }
    return false;
}

std::optional<SyntaxNode> Parameter::nameToken() const {
    if (isThisField()) {
        bool seenDot = false;
        for (auto& c : node.children()) {
            if (isTrivia(c.kind())) continue;
            if (c.kind() == SyntaxKind::Dot) { seenDot = true; continue; }
            if (seenDot && c.kind() == SyntaxKind::Identifier) return c;
        }
        return std::nullopt;
    }
    bool seenType = false;
    for (auto& c : node.children()) {
        if (isTrivia(c.kind())) continue;
        if (c.kind() == SyntaxKind::TypeRef) { seenType = true; continue; }
        if (seenType && c.kind() == SyntaxKind::Identifier) return c;
    }
    return std::nullopt;
}

std::optional<std::u16string> Parameter::nameText() const {
    if (auto t = nameToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}

std::optional<TypeReference> Parameter::typeReference() const {
    if (auto tr = firstChildNode(node, SyntaxKind::TypeRef)) return TypeReference::cast(*tr);
    return std::nullopt;
}

std::optional<DefaultValue> Parameter::defaultValue() const {
    if (auto d = firstChildNode(node, SyntaxKind::DefaultValue)) return DefaultValue::cast(*d);
    return std::nullopt;
}

// === ParameterList ===

std::vector<Parameter> ParameterList::parameters() const {
    std::vector<Parameter> out;
    for (auto& c : node.children()) {
        if (auto p = Parameter::cast(c)) out.push_back(*p);
    }
    return out;
}

// === FuncDecl ===

std::optional<VisibilityModifier> FuncDecl::visibilityModifier() const {
    return visibilityOfDeclNode(node);
}

Visibility FuncDecl::visibility() const {
    return visibilityOfDecl(node);
}

std::optional<SyntaxNode> FuncDecl::nameToken() const {
    for (auto& c : node.children()) {
        if (isTrivia(c.kind()) || c.kind() == SyntaxKind::VisibilityModifier) continue;
        if (c.kind() == SyntaxKind::Identifier) return c;
        break;
    }
    return std::nullopt;
}

std::optional<std::u16string> FuncDecl::nameText() const {
    if (auto t = nameToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}

std::optional<ParameterList> FuncDecl::parameterList() const {
    if (auto p = firstChildNode(node, SyntaxKind::ParamList)) return ParameterList::cast(*p);
    return std::nullopt;
}

std::vector<Parameter> FuncDecl::parameters() const {
    if (auto pl = parameterList()) return pl->parameters();
    return {};
}

std::optional<ReturnType> FuncDecl::returnType() const {
    if (auto rt = firstChildNode(node, SyntaxKind::ReturnType)) return ReturnType::cast(*rt);
    return std::nullopt;
}

std::optional<Block> FuncDecl::body() const {
    if (auto b = firstChildNode(node, SyntaxKind::Block)) return Block::cast(*b);
    return std::nullopt;
}

bool FuncDecl::isShorthand() const {
    if (body().has_value()) return false;
    for (auto& c : node.children()) {
        if (c.kind() == SyntaxKind::Semi) return true;
    }
    return false;
}

// === FieldDecl ===

std::optional<VisibilityModifier> FieldDecl::visibilityModifier() const {
    return visibilityOfDeclNode(node);
}

Visibility FieldDecl::visibility() const {
    return visibilityOfDecl(node);
}

bool FieldDecl::isWeak() const {
    for (auto& c : node.children()) {
        if (c.kind() == SyntaxKind::KwWeak) return true;
        if (c.kind() == SyntaxKind::TypeRef) break;
    }
    return false;
}

std::optional<TypeReference> FieldDecl::typeReference() const {
    if (auto tr = firstChildNode(node, SyntaxKind::TypeRef)) return TypeReference::cast(*tr);
    return std::nullopt;
}

std::optional<SyntaxNode> FieldDecl::nameToken() const {
    bool seenType = false;
    for (auto& c : node.children()) {
        if (isTrivia(c.kind())) continue;
        if (c.kind() == SyntaxKind::TypeRef) { seenType = true; continue; }
        if (seenType && c.kind() == SyntaxKind::Identifier) return c;
    }
    return std::nullopt;
}

std::optional<std::u16string> FieldDecl::nameText() const {
    if (auto t = nameToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}

std::optional<DefaultValue> FieldDecl::defaultValue() const {
    if (auto d = firstChildNode(node, SyntaxKind::DefaultValue)) return DefaultValue::cast(*d);
    return std::nullopt;
}

// === MemberList ===

std::vector<FieldDecl> MemberList::fields() const {
    std::vector<FieldDecl> out;
    for (auto& c : node.children()) {
        if (auto f = FieldDecl::cast(c)) out.push_back(*f);
    }
    return out;
}

std::vector<FuncDecl> MemberList::methods() const {
    std::vector<FuncDecl> out;
    for (auto& c : node.children()) {
        if (auto f = FuncDecl::cast(c)) out.push_back(*f);
    }
    return out;
}

// === StructDecl / ClassDecl ===

std::optional<VisibilityModifier> StructDecl::visibilityModifier() const { return visibilityOfDeclNode(node); }
Visibility StructDecl::visibility() const { return visibilityOfDecl(node); }
std::optional<SyntaxNode> StructDecl::nameToken() const { return firstIdentAfterKeyword(node, SyntaxKind::KwStruct); }
std::optional<std::u16string> StructDecl::nameText() const {
    if (auto t = nameToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}
std::optional<MemberList> StructDecl::memberList() const {
    if (auto m = firstChildNode(node, SyntaxKind::MemberList)) return MemberList::cast(*m);
    return std::nullopt;
}
std::vector<FieldDecl> StructDecl::fields() const {
    if (auto ml = memberList()) return ml->fields();
    return {};
}
std::vector<FuncDecl> StructDecl::methods() const {
    if (auto ml = memberList()) return ml->methods();
    return {};
}

std::optional<VisibilityModifier> ClassDecl::visibilityModifier() const { return visibilityOfDeclNode(node); }
Visibility ClassDecl::visibility() const { return visibilityOfDecl(node); }
std::optional<SyntaxNode> ClassDecl::nameToken() const { return firstIdentAfterKeyword(node, SyntaxKind::KwClass); }
std::optional<std::u16string> ClassDecl::nameText() const {
    if (auto t = nameToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}
std::optional<MemberList> ClassDecl::memberList() const {
    if (auto m = firstChildNode(node, SyntaxKind::MemberList)) return MemberList::cast(*m);
    return std::nullopt;
}
std::vector<FieldDecl> ClassDecl::fields() const {
    if (auto ml = memberList()) return ml->fields();
    return {};
}
std::vector<FuncDecl> ClassDecl::methods() const {
    if (auto ml = memberList()) return ml->methods();
    return {};
}

// === ImportPath / ImportDecl ===

bool ImportPath::isPackage() const {
    for (auto& c : node.children()) {
        if (isTrivia(c.kind())) continue;
        if (c.kind() == SyntaxKind::At) return true;
        break;
    }
    return false;
}

std::vector<SyntaxNode> ImportPath::segmentTokens() const {
    std::vector<SyntaxNode> out;
    for (auto& c : node.children()) {
        if (isTrivia(c.kind()) || !c.isToken()) continue;
        if (c.kind() == SyntaxKind::Identifier) out.push_back(c);
    }
    return out;
}

std::vector<std::u16string> ImportPath::segments() const {
    std::vector<std::u16string> out;
    for (auto& t : segmentTokens()) out.emplace_back(t.tokenText());
    return out;
}

std::optional<ImportPath> ImportDecl::importPath() const {
    if (auto p = firstChildNode(node, SyntaxKind::ImportPath)) return ImportPath::cast(*p);
    return std::nullopt;
}

bool ImportDecl::isPackage() const {
    if (auto p = importPath()) return p->isPackage();
    return false;
}

std::optional<SyntaxNode> ImportDecl::aliasToken() const {
    bool seenImport = false;
    for (auto& c : node.children()) {
        if (isTrivia(c.kind())) continue;
        if (c.kind() == SyntaxKind::KwImport) { seenImport = true; continue; }
        if (!seenImport) continue;
        if (c.kind() == SyntaxKind::Identifier) return c;
        // Anything else after `import` (including `ImportPath`) means this is the
        // bare-path form with no alias.
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::u16string> ImportDecl::aliasText() const {
    if (auto t = aliasToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}

std::vector<std::u16string> ImportDecl::pathSegments() const {
    if (auto p = importPath()) return p->segments();
    return {};
}

std::optional<std::u16string> ImportDecl::namespaceName() const {
    if (aliasText()) return std::nullopt;
    auto segs = pathSegments();
    if (segs.empty()) return std::nullopt;
    return segs.back();
}

std::u16string ImportDecl::modulePath() const {
    std::u16string out;
    if (isPackage()) out.push_back(u'@');
    auto segs = pathSegments();
    for (size_t i = 0; i < segs.size(); ++i) {
        if (i > 0) out.push_back(u'.');
        out += segs[i];
    }
    return out;
}

// === SourceFile ===

std::vector<ImportDecl> SourceFile::imports() const {
    std::vector<ImportDecl> out;
    for (auto& c : node.children()) {
        if (auto i = ImportDecl::cast(c)) out.push_back(*i);
    }
    return out;
}

std::vector<FuncDecl> SourceFile::functions() const {
    std::vector<FuncDecl> out;
    for (auto& c : node.children()) {
        if (auto f = FuncDecl::cast(c)) out.push_back(*f);
    }
    return out;
}

std::vector<StructDecl> SourceFile::structs() const {
    std::vector<StructDecl> out;
    for (auto& c : node.children()) {
        if (auto s = StructDecl::cast(c)) out.push_back(*s);
    }
    return out;
}

std::vector<ClassDecl> SourceFile::classes() const {
    std::vector<ClassDecl> out;
    for (auto& c : node.children()) {
        if (auto cl = ClassDecl::cast(c)) out.push_back(*cl);
    }
    return out;
}

std::vector<TypedVarDeclStatement> SourceFile::topLevelVarDecls() const {
    std::vector<TypedVarDeclStatement> out;
    for (auto& c : node.children()) {
        if (auto v = TypedVarDeclStatement::cast(c)) out.push_back(*v);
    }
    return out;
}

}  // namespace ast
