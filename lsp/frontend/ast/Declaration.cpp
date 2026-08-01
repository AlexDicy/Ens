#include "Declaration.h"

#include "semantic/Literals.h"

namespace ast {

// === VisibilityModifier ===

Visibility VisibilityModifier::visibility() const {
    for (auto& c : node.children()) {
        if (isTrivia(c.kind()) || !c.isToken()) continue;
        switch (c.kind()) {
            case SyntaxKind::KwPrivate:   return Visibility::Private;
            case SyntaxKind::KwProtected: return Visibility::Protected;
            case SyntaxKind::KwPublic:    return Visibility::Public;
            case SyntaxKind::KwExport:    return Visibility::Export;
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

// True if the decl node has a direct modifier token of the given kind (e.g. KwAbstract on a
// class, KwOverride on a method). Only scans immediate token children, so nested bodies are safe.
static bool hasDirectToken(const SyntaxNode& decl, SyntaxKind kind) {
    for (auto& c : decl.children()) {
        if (c.isToken() && c.kind() == kind) return true;
    }
    return false;
}

static std::optional<TypeParamList> typeParamListOf(const SyntaxNode& decl) {
    if (auto n = firstChildNode(decl, SyntaxKind::TypeParamList)) return TypeParamList::cast(*n);
    return std::nullopt;
}

// === TypeParam / TypeParamList ===

std::optional<SyntaxNode> TypeParam::nameToken() const {
    for (auto& c : node.children()) {
        if (!isTrivia(c.kind()) && c.kind() == SyntaxKind::Identifier) return c;
    }
    return std::nullopt;
}

std::optional<std::u16string> TypeParam::nameText() const {
    if (auto t = nameToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}

std::optional<TypeReference> TypeParam::bound() const {
    if (auto tr = firstChildNode(node, SyntaxKind::TypeRef)) return TypeReference::cast(*tr);
    return std::nullopt;
}

std::vector<TypeReference> TypeParam::bounds() const {
    std::vector<TypeReference> out;
    for (auto& c : node.children()) {
        if (auto tr = TypeReference::cast(c)) out.push_back(*tr);
    }
    return out;
}

std::vector<TypeParam> TypeParamList::params() const {
    std::vector<TypeParam> out;
    for (auto& c : node.children()) {
        if (auto p = TypeParam::cast(c)) out.push_back(*p);
    }
    return out;
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

bool Parameter::isOut() const {
    for (auto& c : node.children()) {
        if (isTrivia(c.kind())) continue;
        if (c.kind() == SyntaxKind::KwOut) return true;
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
        if (c.kind() == SyntaxKind::KwOut) continue;
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
        if (c.kind() == SyntaxKind::KwOverride || c.kind() == SyntaxKind::KwFinal ||
            c.kind() == SyntaxKind::KwAbstract || c.kind() == SyntaxKind::KwNoreturn)
            continue;  // skip method modifiers
        if (c.kind() == SyntaxKind::Identifier) return c;
        break;
    }
    return std::nullopt;
}

std::optional<std::u16string> FuncDecl::nameText() const {
    if (auto t = nameToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}

std::optional<TypeParamList> FuncDecl::typeParamList() const { return typeParamListOf(node); }
std::vector<TypeParam> FuncDecl::typeParams() const {
    if (auto l = typeParamList()) return l->params();
    return {};
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

bool FuncDecl::isConstructor() const { return hasDirectToken(node, SyntaxKind::KwConstructor); }
bool FuncDecl::isDestructor() const  { return hasDirectToken(node, SyntaxKind::KwDestructor); }
bool FuncDecl::isOverride() const { return hasDirectToken(node, SyntaxKind::KwOverride); }
bool FuncDecl::isFinal() const    { return hasDirectToken(node, SyntaxKind::KwFinal); }
bool FuncDecl::isAbstract() const { return hasDirectToken(node, SyntaxKind::KwAbstract); }
bool FuncDecl::isNoreturn() const { return hasDirectToken(node, SyntaxKind::KwNoreturn); }

std::optional<ThrowsClause> FuncDecl::throwsClause() const {
    if (auto t = firstChildNode(node, SyntaxKind::ThrowsClause)) return ThrowsClause::cast(*t);
    return std::nullopt;
}

bool FuncDecl::isThrows() const { return throwsClause().has_value(); }

std::optional<SyntaxNode> FuncDecl::throwsToken() const {
    auto tc = throwsClause();
    if (!tc) return std::nullopt;
    for (auto& c : tc->node.children()) {
        if (c.isToken() && c.kind() == SyntaxKind::KwThrows) return c;
    }
    return std::nullopt;
}

std::vector<TypeReference> FuncDecl::declaredThrowsTypes() const {
    if (auto tc = throwsClause()) return tc->types();
    return {};
}

std::vector<CatchClause> FuncDecl::catchClauses() const {
    std::vector<CatchClause> out;
    for (auto& c : node.children()) {
        if (auto cc = CatchClause::cast(c)) out.push_back(*cc);
    }
    return out;
}

// === ThrowsClause ===

std::vector<TypeReference> ThrowsClause::types() const {
    std::vector<TypeReference> out;
    for (auto& c : node.children()) {
        if (auto tr = TypeReference::cast(c)) out.push_back(*tr);
    }
    return out;
}

// === CatchClause ===

std::optional<TypeReference> CatchClause::typeReference() const {
    if (auto tr = firstChildNode(node, SyntaxKind::TypeRef)) return TypeReference::cast(*tr);
    return std::nullopt;
}

std::optional<SyntaxNode> CatchClause::nameToken() const {
    bool seenType = false;
    for (auto& c : node.children()) {
        if (isTrivia(c.kind())) continue;
        if (c.kind() == SyntaxKind::TypeRef) { seenType = true; continue; }
        if (seenType && c.kind() == SyntaxKind::Identifier) return c;
    }
    return std::nullopt;
}

std::optional<std::u16string> CatchClause::nameText() const {
    if (auto t = nameToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}

std::optional<Block> CatchClause::body() const {
    if (auto b = firstChildNode(node, SyntaxKind::Block)) return Block::cast(*b);
    return std::nullopt;
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
std::optional<TypeParamList> StructDecl::typeParamList() const { return typeParamListOf(node); }
std::vector<TypeParam> StructDecl::typeParams() const {
    if (auto l = typeParamList()) return l->params();
    return {};
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
std::optional<TypeParamList> ClassDecl::typeParamList() const { return typeParamListOf(node); }
std::vector<TypeParam> ClassDecl::typeParams() const {
    if (auto l = typeParamList()) return l->params();
    return {};
}
std::optional<SyntaxNode> ClassDecl::baseClassToken() const {
    return firstIdentAfterKeyword(node, SyntaxKind::KwExtends);
}
std::optional<std::u16string> ClassDecl::baseClassName() const {
    if (auto t = baseClassToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}
std::vector<TypeReference> ClassDecl::baseTypeArguments() const {
    std::vector<TypeReference> out;
    bool afterExtends = false;
    for (auto& c : node.children()) {
        if (c.kind() == SyntaxKind::KwExtends) { afterExtends = true; continue; }
        if (!afterExtends || c.kind() != SyntaxKind::TypeArgList) continue;
        for (auto& a : c.children()) {
            if (auto tr = TypeReference::cast(a)) out.push_back(*tr);
        }
        break;
    }
    return out;
}
std::optional<ImplementsClause> ClassDecl::implementsClause() const {
    if (auto n = firstChildNode(node, SyntaxKind::ImplementsClause)) return ImplementsClause::cast(*n);
    return std::nullopt;
}
std::vector<TypeReference> ClassDecl::implementedInterfaceRefs() const {
    if (auto ic = implementsClause()) return ic->types();
    return {};
}
bool ClassDecl::isAbstract() const { return hasDirectToken(node, SyntaxKind::KwAbstract); }
bool ClassDecl::isFinal() const    { return hasDirectToken(node, SyntaxKind::KwFinal); }
bool ClassDecl::isSealed() const   { return hasDirectToken(node, SyntaxKind::KwSealed); }

// === ImplementsClause ===

std::vector<TypeReference> ImplementsClause::types() const {
    std::vector<TypeReference> out;
    for (auto& c : node.children()) {
        if (auto tr = TypeReference::cast(c)) out.push_back(*tr);
    }
    return out;
}

// === InterfaceDecl ===

std::optional<VisibilityModifier> InterfaceDecl::visibilityModifier() const { return visibilityOfDeclNode(node); }
Visibility InterfaceDecl::visibility() const { return visibilityOfDecl(node); }
std::optional<SyntaxNode> InterfaceDecl::nameToken() const {
    return firstIdentAfterKeyword(node, SyntaxKind::KwInterface);
}
std::optional<std::u16string> InterfaceDecl::nameText() const {
    if (auto t = nameToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}
std::optional<TypeParamList> InterfaceDecl::typeParamList() const { return typeParamListOf(node); }
std::vector<TypeParam> InterfaceDecl::typeParams() const {
    if (auto l = typeParamList()) return l->params();
    return {};
}
std::optional<MemberList> InterfaceDecl::memberList() const {
    if (auto m = firstChildNode(node, SyntaxKind::MemberList)) return MemberList::cast(*m);
    return std::nullopt;
}
std::vector<FieldDecl> InterfaceDecl::fields() const {
    if (auto ml = memberList()) return ml->fields();
    return {};
}
std::vector<FuncDecl> InterfaceDecl::methods() const {
    if (auto ml = memberList()) return ml->methods();
    return {};
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
    auto segs = pathSegments();
    for (size_t i = 0; i < segs.size(); ++i) {
        if (i > 0) out.push_back(u'.');
        out += segs[i];
    }
    return out;
}

// === EnumMember ===

std::optional<SyntaxNode> EnumMember::nameToken() const {
    return firstChildNode(node, SyntaxKind::Identifier);
}

std::optional<std::u16string> EnumMember::nameText() const {
    if (auto t = nameToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}

std::optional<DefaultValue> EnumMember::value() const {
    if (auto d = firstChildNode(node, SyntaxKind::DefaultValue)) return DefaultValue::cast(*d);
    return std::nullopt;
}

// === EnumDecl ===

std::optional<VisibilityModifier> EnumDecl::visibilityModifier() const { return visibilityOfDeclNode(node); }
Visibility EnumDecl::visibility() const { return visibilityOfDecl(node); }
std::optional<SyntaxNode> EnumDecl::nameToken() const { return firstIdentAfterKeyword(node, SyntaxKind::KwEnum); }
std::optional<std::u16string> EnumDecl::nameText() const {
    if (auto t = nameToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}
std::vector<EnumMember> EnumDecl::members() const {
    std::vector<EnumMember> out;
    for (auto& c : node.children()) {
        if (auto m = EnumMember::cast(c)) out.push_back(*m);
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

// === TestDecl ===

std::optional<SyntaxNode> TestDecl::descriptionToken() const {
    for (auto& c : node.children()) {
        if (!isTrivia(c.kind()) && c.kind() == SyntaxKind::StringLiteral) return c;
    }
    return std::nullopt;
}

std::optional<std::u16string> TestDecl::rawDescriptionLiteral() const {
    if (auto t = descriptionToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}

std::optional<std::u16string> TestDecl::descriptionText() const {
    auto raw = rawDescriptionLiteral();
    if (!raw) return std::nullopt;
    size_t lo = !raw->empty() && (*raw)[0] == u'"' ? 1 : 0;
    size_t hi = raw->size() > lo && raw->back() == u'"' ? raw->size() - 1 : raw->size();
    std::u16string out;
    for (size_t i = lo; i < hi; ++i) {
        char16_t c = (*raw)[i];
        uint32_t scalar = c;
        if (c == u'\\' && i + 1 < hi) {
            size_t next;
            scalar = decodeEscapeSequence(*raw, i, hi, next);
            i = next - 1;
        }
        if (scalar <= 0xFFFF) {
            out.push_back(static_cast<char16_t>(scalar));
        } else {
            uint32_t v = scalar - 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (v >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (v & 0x3FF)));
        }
    }
    return out;
}

std::optional<Block> TestDecl::body() const {
    if (auto b = firstChildNode(node, SyntaxKind::Block)) return Block::cast(*b);
    return std::nullopt;
}

std::vector<FuncDecl> SourceFile::functions() const {
    std::vector<FuncDecl> out;
    for (auto& c : node.children()) {
        if (auto f = FuncDecl::cast(c)) out.push_back(*f);
    }
    return out;
}

std::vector<TestDecl> SourceFile::tests() const {
    std::vector<TestDecl> out;
    for (auto& c : node.children()) {
        if (auto t = TestDecl::cast(c)) out.push_back(*t);
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

std::vector<InterfaceDecl> SourceFile::interfaces() const {
    std::vector<InterfaceDecl> out;
    for (auto& c : node.children()) {
        if (auto i = InterfaceDecl::cast(c)) out.push_back(*i);
    }
    return out;
}

std::vector<EnumDecl> SourceFile::enums() const {
    std::vector<EnumDecl> out;
    for (auto& c : node.children()) {
        if (auto e = EnumDecl::cast(c)) out.push_back(*e);
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

std::vector<ExternalTypeDecl> SourceFile::externalTypes() const {
    std::vector<ExternalTypeDecl> out;
    for (auto& c : node.children()) {
        if (auto e = ExternalTypeDecl::cast(c)) out.push_back(*e);
    }
    return out;
}

std::vector<ExternalBlock> SourceFile::externalBlocks() const {
    std::vector<ExternalBlock> out;
    for (auto& c : node.children()) {
        if (auto e = ExternalBlock::cast(c)) out.push_back(*e);
    }
    return out;
}

// === ExternalTypeDecl ===

std::optional<VisibilityModifier> ExternalTypeDecl::visibilityModifier() const {
    return visibilityOfDeclNode(node);
}

Visibility ExternalTypeDecl::visibility() const {
    return visibilityOfDecl(node);
}

std::optional<SyntaxNode> ExternalTypeDecl::nameToken() const {
    return firstIdentAfterKeyword(node, SyntaxKind::KwType);
}

std::optional<std::u16string> ExternalTypeDecl::nameText() const {
    if (auto t = nameToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}

// === ExternalFuncDecl ===

std::optional<SyntaxNode> ExternalFuncDecl::nameToken() const {
    for (auto& c : node.children()) {
        if (isTrivia(c.kind())) continue;
        if (c.kind() == SyntaxKind::Identifier) return c;
        break;
    }
    return std::nullopt;
}

std::optional<std::u16string> ExternalFuncDecl::nameText() const {
    if (auto t = nameToken()) return std::u16string(t->tokenText());
    return std::nullopt;
}

std::optional<ParameterList> ExternalFuncDecl::parameterList() const {
    if (auto p = firstChildNode(node, SyntaxKind::ParamList)) return ParameterList::cast(*p);
    return std::nullopt;
}

std::vector<Parameter> ExternalFuncDecl::parameters() const {
    if (auto pl = parameterList()) return pl->parameters();
    return {};
}

std::optional<ReturnType> ExternalFuncDecl::returnType() const {
    if (auto rt = firstChildNode(node, SyntaxKind::ReturnType)) return ReturnType::cast(*rt);
    return std::nullopt;
}

// === ExternalBlock ===

std::optional<VisibilityModifier> ExternalBlock::visibilityModifier() const {
    return visibilityOfDeclNode(node);
}

Visibility ExternalBlock::visibility() const {
    return visibilityOfDecl(node);
}

std::optional<std::u16string> ExternalBlock::libraryName() const {
    auto spec = firstChildNode(node, SyntaxKind::LibrarySpec);
    if (!spec) return std::nullopt;
    for (auto& c : spec->children()) {
        if (c.kind() == SyntaxKind::Identifier) {
            return std::u16string{c.tokenText()};
        }
    }
    return std::nullopt;
}

std::vector<ExternalFuncDecl> ExternalBlock::declarations() const {
    std::vector<ExternalFuncDecl> out;
    for (auto& c : node.children()) {
        if (auto e = ExternalFuncDecl::cast(c)) out.push_back(*e);
    }
    return out;
}

}  // namespace ast
