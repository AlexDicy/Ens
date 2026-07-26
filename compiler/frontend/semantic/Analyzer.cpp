#include "Analyzer.h"

#include <algorithm>
#include <cassert>
#include <unordered_set>
#include "../diagnostics/Diagnostic.h"
#include "../diagnostics/DiagnosticSink.h"
#include "../parser/Parser.h"
#include "Literals.h"
#include "Prelude.h"
#include "ThrowsAnalyzer.h"

// Sentinel for a method known to need a vtable slot before final indices are assigned.
static constexpr int VTSLOT_PENDING = -2;

static size_t requiredArgCount(Symbol* sym);

static Visibility toSemanticVisibility(ast::Visibility v) {
    switch (v) {
        case ast::Visibility::Private:   return Visibility::Private;
        case ast::Visibility::Protected: return Visibility::Protected;
        case ast::Visibility::Public:    return Visibility::Public;
        case ast::Visibility::Export:    return Visibility::Export;
    }
    return Visibility::Private;
}

// The visibility of a member that follows its containing type: such a member is
// accessible wherever the type is nameable. A private type is nameable throughout
// its file, which for members is exactly the protected floor.
static Visibility followedMemberVisibility(Visibility container) {
    if (container == Visibility::Private) return Visibility::Protected;
    return container;
}

// The declaring StructInfo for identity/visibility questions: an instantiation
// answers through its template.
static StructInfo* declarationAuthority(StructInfo* si) {
    return si && si->templateOf ? si->templateOf : si;
}

static const StructInfo* declarationAuthority(const StructInfo* si) {
    return si && si->templateOf ? si->templateOf : si;
}

static const char* visibilityWord(Visibility v) {
    switch (v) {
        case Visibility::Private:   return "private";
        case Visibility::Protected: return "protected";
        case Visibility::Public:    return "public";
        case Visibility::Export:    return "export";
    }
    return "private";
}

static std::string asciiOf(std::u16string_view s) {
    std::string r;
    r.reserve(s.size());
    for (char16_t c : s) r.push_back(c < 128 ? static_cast<char>(c) : '?');
    return r;
}

// The declared kind of an existing type as it reads in a diagnostic.
static std::string typeKindWord(const Type* t) {
    if (!t) return "type";
    if (t->isEnum()) return "enum";
    if (t->isExternal()) return "external type";
    if (t->isInterface()) return "interface";
    if (t->isStruct()) return "struct";
    if (t->isClass()) return "class";
    return "type";
}

// =========================================================
// Construction / scaffolding
// =========================================================

struct Analyzer::PreludeData {
    SourceFile source;
    DiagnosticSink sink;
    GreenElementPtr cstRoot;
    std::unique_ptr<SyntaxNode> rootNode;
    std::unique_ptr<Analyzer> analyzer;

    PreludeData()
        : source("<prelude>", std::u16string(kPreludeSource)) {}
};

Analyzer::Analyzer(const SourceFile& src, DiagnosticSink& s)
    : source(src), sink(s),
      ownedTypeCtx(std::make_unique<TypeContext>()),
      typeCtx(*ownedTypeCtx),
      modulePath_(u"main") {
    auto scope = std::make_unique<Scope>(nullptr);
    globalScope = scope.get();
    currentScope = globalScope;
    ownedScopes.push_back(std::move(scope));
    registerBuiltins();
    bootstrapPrelude();
}

Analyzer::Analyzer(const SourceFile& src, DiagnosticSink& s,
                   TypeContext& sharedContext, std::u16string mp, std::u16string packagePrefix)
    : source(src), sink(s),
      ownedTypeCtx(),
      typeCtx(sharedContext),
      modulePath_(std::move(mp)),
      packagePrefix_(std::move(packagePrefix)) {
    auto scope = std::make_unique<Scope>(nullptr);
    globalScope = scope.get();
    currentScope = globalScope;
    ownedScopes.push_back(std::move(scope));
    registerBuiltins();
}

Analyzer::~Analyzer() = default;

void Analyzer::bootstrapPrelude() {
    // Compile the prelude into this analyzer's own TypeContext via a nested
    // shared-context analyzer (which performs no bootstrap, breaking recursion),
    prelude_ = std::make_unique<PreludeData>();
    Parser parser(prelude_->source.getSource(), prelude_->sink);
    prelude_->cstRoot = parser.parseSourceFile();
    prelude_->rootNode = SyntaxNode::makeRoot(prelude_->cstRoot.get());
    prelude_->analyzer = std::make_unique<Analyzer>(
        prelude_->source, prelude_->sink, typeCtx, std::u16string(kPreludeModulePath));
    prelude_->analyzer->collectDeclarations(*prelude_->rootNode);
    prelude_->analyzer->analyzeBodies();
    importPrelude();
}

void Analyzer::importPrelude() {
    Type* err = typeCtx.lookupNamedType(std::u16string(kPreludeModulePath), u"Error");
    if (!err || !err->structInfo) return;
    errorClassInfo_ = err->structInfo;
    if (globalScope && !globalScope->lookupLocal(u"Error")) {
        Symbol* s = makeSymbol(SymbolKind::Variable, std::u16string(u"Error"), err, 0);
        globalScope->define(s);
    }
}

void Analyzer::registerBuiltins() {
    Type* voidTy   = typeCtx.getPrimitive(TypeKind::Void);
    Type* stringTy = typeCtx.getPrimitive(TypeKind::String);

    Symbol* printSym = makeSymbol(SymbolKind::Function, std::u16string(u"print"), nullptr, 0);
    printSym->returnType = voidTy;
    printSym->paramTypes = {stringTy};
    printSym->isBuiltin = true;
    globalScope->define(printSym);

    Symbol* panicSym = makeSymbol(SymbolKind::Function, std::u16string(u"panic"), nullptr, 0);
    panicSym->returnType = voidTy;
    panicSym->paramTypes = {stringTy};
    panicSym->isBuiltin = true;
    panicSym->isNoreturn = true;
    globalScope->define(panicSym);
}

Symbol* Analyzer::makeSymbol(SymbolKind k, std::u16string n, Type* t, uint32_t offset) {
    auto [line, column] = source.offsetToPosition(offset);
    auto s = std::make_unique<Symbol>(k, std::move(n), t, line, column);
    s->modulePath = modulePath_;
    Symbol* raw = s.get();
    ownedSymbols.push_back(std::move(s));
    return raw;
}

Scope* Analyzer::pushScope() {
    auto scope = std::make_unique<Scope>(currentScope);
    Scope* raw = scope.get();
    ownedScopes.push_back(std::move(scope));
    currentScope = raw;
    return raw;
}

void Analyzer::popScope() {
    if (currentScope && currentScope->parent) currentScope = currentScope->parent;
}

int Analyzer::lineOf(uint32_t offset) const   { return source.offsetToPosition(offset).first; }
int Analyzer::columnOf(uint32_t offset) const { return source.offsetToPosition(offset).second; }

void Analyzer::error(uint32_t offset, int length, std::string message) {
    auto [line, column] = source.offsetToPosition(offset);
    sink.error({line, column, length > 0 ? length : 1}, std::move(message));
}

void Analyzer::errorAtNode(const SyntaxNode& node, std::string message) {
    auto [offset, length] = node.contentRange();
    error(offset, static_cast<int>(length), std::move(message));
}

Symbol* Analyzer::globalSymbol(const std::u16string& name) const {
    if (!globalScope) return nullptr;
    return globalScope->lookupLocal(name);
}

Visibility Analyzer::topLevelVisibility(const std::optional<ast::VisibilityModifier>& modifier,
                                        const std::string& declName) {
    if (!modifier) return Visibility::Private;
    Visibility v = toSemanticVisibility(modifier->visibility());
    if (v == Visibility::Protected) {
        errorAtNode(modifier->node, "'protected' is not allowed on a top-level declaration; it "
            "is only meaningful for class and struct members. Use 'public' to share '" +
            declName + "' with the rest of the package, or leave it unmarked to keep it "
            "private to this file.");
        return Visibility::Private;
    }
    return v;
}

Visibility Analyzer::memberVisibility(const std::optional<ast::VisibilityModifier>& modifier,
                                      Visibility defaultVisibility, StructInfo* owner,
                                      const std::string& memberKindWord,
                                      const std::u16string& memberName) {
    if (!modifier) return defaultVisibility;
    Visibility v = toSemanticVisibility(modifier->visibility());
    if (v != Visibility::Protected && owner &&
        visibilityTier(v) > visibilityTier(owner->visibility)) {
        errorAtNode(modifier->node, memberKindWord + " '" + asciiOf(memberName) +
            "' is marked '" + visibilityWord(v) + "' but '" + asciiOf(owner->name) + "' is " +
            (owner->visibility == Visibility::Private ? "private to its file"
                                                      : std::string("only '") +
                                                        visibilityWord(owner->visibility) + "'") +
            "; a member cannot be more visible than the type that contains it. Raise '" +
            asciiOf(owner->name) + "' to '" + visibilityWord(v) + "', or lower '" +
            asciiOf(memberName) + "'.");
        return owner->visibility;
    }
    return v;
}

bool Analyzer::isTopLevelVisibleFrom(Visibility v, const std::u16string& declModulePath,
                                     const std::u16string& declPackagePrefix) const {
    if (declModulePath == modulePath_) return true;
    if (v == Visibility::Export) return true;
    if (v == Visibility::Public) return declPackagePrefix == packagePrefix_;
    return false;
}

// Whether a type is nameable from this module. Primitives and other built-in
// forms (no StructInfo) are visible everywhere.
bool Analyzer::isTypeVisibleFrom(const Type* t) const {
    if (!t || !t->structInfo) return true;
    const StructInfo* si = declarationAuthority(t->structInfo);
    return isTopLevelVisibleFrom(si->visibility, si->modulePath, si->packagePrefix);
}

std::string Analyzer::invisibleSymbolMessage(const std::string& kindWord,
                                             const std::u16string& name, Visibility v,
                                             const std::u16string& declModulePath,
                                             const std::u16string& declPackagePrefix) const {
    bool samePackage = declPackagePrefix == packagePrefix_;
    std::string shownName = asciiOf(name);
    if (v == Visibility::Public && !samePackage) {
        std::string packageName = declPackagePrefix.empty()
            ? std::string("its package") : "package '" + asciiOf(declPackagePrefix) + "'";
        return kindWord + " '" + shownName + "' is public inside " + packageName +
            " but not exported. Mark it 'export' in module '" + asciiOf(declModulePath) +
            "' to use it from another package.";
    }
    std::string fix = samePackage
        ? "Mark it 'public' to use it from other modules in the package."
        : "Mark it 'export' to use it from another package.";
    return kindWord + " '" + shownName + "' is private to module '" + asciiOf(declModulePath) +
        "'; it can only be used inside that file. " + fix;
}

std::string Analyzer::invisibleTypeMessage(const std::u16string& name, const Type* t) const {
    const StructInfo* si = t && t->structInfo ? declarationAuthority(t->structInfo) : nullptr;
    if (!si) return "Type '" + asciiOf(name) + "' is not visible here.";
    return invisibleSymbolMessage("Type", name, si->visibility, si->modulePath,
                                  si->packagePrefix);
}

namespace {

bool isIntegerLiteralKind(SyntaxKind k, bool allowLong) {
    return k == SyntaxKind::IntLiteral || (allowLong && k == SyntaxKind::LongLiteral);
}

const ast::LiteralExpression* asIntegerLiteralChild(const ast::Expression& e, bool allowLong) {
    if (auto lit = e.asLiteral()) {
        static thread_local std::optional<ast::LiteralExpression> hold;
        if (!isIntegerLiteralKind(lit->literalKind(), allowLong)) return nullptr;
        hold = lit;
        return &*hold;
    }
    if (auto pre = e.asPrefix()) {
        auto op = pre->operatorToken();
        if (!op) return nullptr;
        if (op->kind() != SyntaxKind::Plus && op->kind() != SyntaxKind::Minus) return nullptr;
        auto operand = pre->operand();
        if (!operand) return nullptr;
        if (auto lit = operand->asLiteral()) {
            if (!isIntegerLiteralKind(lit->literalKind(), allowLong)) return nullptr;
            static thread_local std::optional<ast::LiteralExpression> hold;
            hold = lit;
            return &*hold;
        }
    }
    return nullptr;
}

const ast::LiteralExpression* asIntLiteralChild(const ast::Expression& e) {
    return asIntegerLiteralChild(e, /*allowLong=*/false);
}

bool literalIsNegative(const ast::Expression& e) {
    if (auto pre = e.asPrefix()) {
        if (auto op = pre->operatorToken()) return op->kind() == SyntaxKind::Minus;
    }
    return false;
}

std::string integerRangeString(Type* target) {
    if (!target) return "";
    switch (target->kind) {
        case TypeKind::Byte:   return "0..255";
        case TypeKind::Short:  return "-32768..32767";
        case TypeKind::Int:    return "-2147483648..2147483647";
        case TypeKind::Long:   return "-9223372036854775808..9223372036854775807";
        case TypeKind::UShort: return "0..65535";
        case TypeKind::UInt:   return "0..4294967295";
        case TypeKind::ULong:  return "0..18446744073709551615";
        case TypeKind::Char:   return "0..1114111";
        default:
            assert(false && "integerRangeString called on non-integer type");
            return "";
    }
}

// The 64-bit value of an integer-constant expression: an int or long literal by
// its value, a char literal by its codepoint, behind an optional '+'/'-' sign.
// Returns nullopt when the expression is not such a constant.
std::optional<int64_t> integerConstantValue(const ast::Expression& e) {
    if (auto lit = e.asLiteral()) {
        if (lit->literalKind() == SyntaxKind::CharLiteral) {
            if (auto tok = lit->token())
                return static_cast<int64_t>(parseCharLiteralCodepoint(tok->tokenText()));
            return std::nullopt;
        }
    }
    const ast::LiteralExpression* lit = asIntegerLiteralChild(e, /*allowLong=*/true);
    if (!lit) return std::nullopt;
    auto tok = lit->token();
    if (!tok) return std::nullopt;
    uint64_t mag = 0;
    if (!parseIntegerLiteralMagnitude(std::u16string(tok->tokenText()), mag)) return std::nullopt;
    return literalIsNegative(e) ? -static_cast<int64_t>(mag) : static_cast<int64_t>(mag);
}

bool literalFitsTarget(bool negative, uint64_t magnitude, Type* target) {
    if (!target) return false;
    switch (target->kind) {
        case TypeKind::Byte:
            return !negative && magnitude <= 255u;
        case TypeKind::Short:
            return negative ? magnitude <= 32768u : magnitude <= 32767u;
        case TypeKind::Int:
            return negative ? magnitude <= uint64_t(2147483648ull)
                            : magnitude <= uint64_t(2147483647ull);
        case TypeKind::Long:
            return negative ? magnitude <= uint64_t(9223372036854775808ull)
                            : magnitude <= uint64_t(9223372036854775807ull);
        case TypeKind::UShort:
            return !negative && magnitude <= 65535u;
        case TypeKind::UInt:
            return !negative && magnitude <= 4294967295u;
        case TypeKind::ULong:
            return !negative;  // any magnitude fits ulong
        case TypeKind::Char:
            return !negative && magnitude <= 0x10FFFFu;
        default:
            assert(false && "literalFitsTarget called on non-integer type");
            return false;
    }
}

}  // namespace

void Analyzer::tryAdaptCharLiteral(const ast::Expression& src, Type* target) {
    if (!target || target->isError()) return;
    if (!target->isInteger()) return;
    if (target->kind == TypeKind::Char) return;  // already char
    auto lit = src.asLiteral();
    if (!lit || lit->literalKind() != SyntaxKind::CharLiteral) return;

    auto tok = lit->token();
    if (!tok) return;
    uint32_t cp = parseCharLiteralCodepoint(tok->tokenText());

    if (!literalFitsTarget(/*negative*/ false, cp, target)) {
        errorAtNode(src.node, "Character literal does not fit in '" + target->toString() +
            "' (range " + integerRangeString(target) + ")");
        analysis.setType(src.node.greenNode(), typeCtx.getError());
        return;
    }
    analysis.setType(src.node.greenNode(), target);
    analysis.setType(lit->node.greenNode(), target);
}

void Analyzer::tryAdaptIntegerLiteral(const ast::Expression& src, Type* target) {
    if (!target || target->isError()) return;
    if (!target->isInteger()) return;
    const ast::LiteralExpression* lit = asIntLiteralChild(src);
    if (!lit) return;

    auto tok = lit->token();
    if (!tok) return;
    std::u16string text(tok->tokenText());
    uint64_t magnitude = 0;
    if (!parseIntegerLiteralMagnitude(text, magnitude)) return;
    bool negative = literalIsNegative(src);

    if (!literalFitsTarget(negative, magnitude, target)) {
        std::string textAscii = asciiOf(text);
        std::string num = (negative ? std::string("-") : std::string("")) + textAscii;
        errorAtNode(src.node, "Literal " + num + " does not fit in '" + target->toString() +
            "' (range " + integerRangeString(target) + ")");
        analysis.setType(src.node.greenNode(), typeCtx.getError());
        return;
    }

    // In-range: retype the literal-bearing nodes so codegen emits at the
    // target's width.
    analysis.setType(src.node.greenNode(), target);
    analysis.setType(lit->node.greenNode(), target);
}

// A switch integer label may be written in any integer-literal form, including a
// typed long literal like `65L`. It adapts to the scrutinee like an int literal:
// its value must fit, and the node is retyped so codegen emits it at the
// scrutinee's width. Unlike tryAdaptIntegerLiteral this accepts long literals,
// which otherwise keep their 'long' type and would be rejected as labels.
void Analyzer::adaptIntegerLiteralLabel(const ast::Expression& src, Type* target) {
    if (!target || target->isError() || !target->isInteger()) return;
    const ast::LiteralExpression* lit = asIntegerLiteralChild(src, /*allowLong=*/true);
    if (!lit) return;

    auto tok = lit->token();
    if (!tok) return;
    std::u16string text(tok->tokenText());
    uint64_t magnitude = 0;
    if (!parseIntegerLiteralMagnitude(text, magnitude)) return;
    bool negative = literalIsNegative(src);

    if (!literalFitsTarget(negative, magnitude, target)) {
        std::string num = (negative ? std::string("-") : std::string("")) + asciiOf(text);
        errorAtNode(src.node, "Literal " + num + " does not fit in '" + target->toString() +
            "' (range " + integerRangeString(target) + ")");
        analysis.setType(src.node.greenNode(), typeCtx.getError());
        return;
    }
    analysis.setType(src.node.greenNode(), target);
    analysis.setType(lit->node.greenNode(), target);
}

Type* Analyzer::numericCommonType(Type* a, Type* b) {
    if (!a || !b) return nullptr;
    if (a->isError() || b->isError()) return nullptr;
    if (a->equals(b)) return a;
    if (a->widensTo(b)) return b;
    if (b->widensTo(a)) return a;
    return nullptr;
}

Type* Analyzer::analyzeExprAdapt(const ast::Expression& expr, Type* target) {
    if (auto al = expr.asArrayLiteral()) {
        Type* t = analyzeArrayLiteralAdapt(*al, target);
        analysis.setType(expr.node.greenNode(), t);
        return t;
    }
    if (auto sl = expr.asStructLiteral()) {
        Type* t = analyzeStructLiteralAdapt(*sl, target);
        analysis.setType(expr.node.greenNode(), t);
        return t;
    }
    Type* t = analyzeExpr(expr);
    if (!target || target->isError() || t->isError()) return t;
    tryAdaptIntegerLiteral(expr, target);
    tryAdaptCharLiteral(expr, target);
    Type* updated = analysis.typeOf(expr.node.greenNode());
    return updated ? updated : t;
}

// =========================================================
// Top-level pipeline
// =========================================================

void Analyzer::analyze(const SyntaxNode& root) {
    collectDeclarations(root);
    bindImports([](const std::u16string&) -> const Analyzer* { return nullptr; });
    checkStructValueCycles();
    analyzeBodies();
    checkSignatureVisibility();
    if (astRoot) {
        ThrowsAnalyzer throwsAnalyzer(*astRoot, analysis, errorClassInfo_);
        throwsAnalyzer.analyze();
        throwsAnalyzer.validate(sink, source);
    }
    for (const auto& o : typeCtx.takeInstantiationOverflows()) {
        sink.error({o.line, o.column, o.length}, o.message);
    }
}

void Analyzer::collectDeclarations(const SyntaxNode& root) {
    registerNames(root);
    resolveSignatures();
    if (astRoot) layoutDeclaredClasses(*astRoot);
    typeCtx.materializeInstantiations();
}

void Analyzer::registerNames(const SyntaxNode& root) {
    auto sf = ast::SourceFile::cast(root);
    if (!sf) return;
    astRoot = sf;

    registerStructNames(*sf);
    registerClassNames(*sf);
    registerInterfaceNames(*sf);
    registerEnumNames(*sf);
    registerExternalTypeNames(*sf);
    rejectTopLevelVariables(*sf);
}

// A `let`, `const`, or typed variable declaration written at file scope parses
// but has no meaning: Ens has no top-level variables. The analyzer would drop it
// silently, so a later use fails far away with 'Undefined name'. Report it here
// at the declaration instead.
void Analyzer::rejectTopLevelVariables(const ast::SourceFile& file) {
    for (auto& child : file.node.children()) {
        std::optional<SyntaxNode> nameTok;
        std::optional<std::u16string> name;
        if (auto v = ast::TypedVarDeclStatement::cast(child)) {
            nameTok = v->nameToken();
            name = v->nameText();
        } else if (auto l = ast::LetStatement::cast(child)) {
            nameTok = l->nameToken();
            name = l->nameText();
        } else {
            continue;
        }
        std::string named = name ? "'" + asciiOf(*name) + "'" : "it";
        errorAtNode(nameTok ? *nameTok : child, "Top-level variables are not supported: " +
            named + " must be declared inside a function. Move it into a function, or "
            "expose it through a function that returns the value, for example "
            "'getValue() -> int { return 3; }'.");
    }
}

void Analyzer::resolveSignatures() {
    if (!astRoot) return;
    auto& sf = *astRoot;

    collectStructs(sf);
    collectInterfaces(sf);
    collectEnums(sf);
    resolveClassBases(sf);
    collectFunctions(sf);
    collectTests(sf);
    collectExternalFunctions(sf);
}

void Analyzer::registerExternalTypeNames(const ast::SourceFile& file) {
    for (auto& ed : file.externalTypes()) {
        auto name = ed.nameText();
        if (!name) continue;
        if (Type* existing = typeCtx.lookupNamedType(modulePath_, *name)) {
            errorAtNode(ed.node, "Duplicate type '" + asciiOf(*name) +
                "'; this file already declares a " + typeKindWord(existing) + " with this name.");
            continue;
        }
        Visibility visibility = Visibility::Private;
        if (auto modifier = ed.visibilityModifier()) {
            Visibility marked = toSemanticVisibility(modifier->visibility());
            if (marked == Visibility::Public || marked == Visibility::Private) {
                visibility = marked;
            } else if (marked == Visibility::Protected) {
                errorAtNode(modifier->node, "An external type may be 'private' or 'public'; "
                    "'protected' is only meaningful for class and struct members. Mark '" +
                    asciiOf(*name) + "' 'public' to share it across the package, or leave it "
                    "unmarked.");
            } else {
                errorAtNode(modifier->node, "An external type may be 'private' or 'public'; it "
                    "can never be exported. Wrap '" + asciiOf(*name) + "' in an Ens type to share "
                    "it beyond the package.");
            }
        }
        Type* t = typeCtx.registerExternalType(modulePath_, *name);
        auto [line, col] = source.offsetToPosition(
            ed.nameToken() ? ed.nameToken()->startOffset() : ed.node.startOffset());
        if (t->structInfo) {
            t->structInfo->line = line;
            t->structInfo->column = col;
            t->structInfo->visibility = visibility;
            t->structInfo->packagePrefix = packagePrefix_;
        }
        analysis.setType(ed.node.greenNode(), t);
    }
}

void Analyzer::collectExternalFunctions(const ast::SourceFile& file) {
    for (auto& block : file.externalBlocks()) {
        if (auto modifier = block.visibilityModifier()) {
            errorAtNode(modifier->node, "An external function block is always private to its "
                "file and cannot carry a visibility modifier; wrap its calls in Ens functions "
                "to share them.");
        }
        auto libName = block.libraryName();
        if (libName && restrictNatives_) {
            bool declared = false;
            for (auto& n : declaredNatives_) { if (n == *libName) { declared = true; break; } }
            if (!declared) {
                errorAtNode(block.node, "External library '" + asciiOf(*libName) +
                    "' is not declared in " + nativeManifestPath_ + ". Add 'native " +
                    asciiOf(*libName) + ";' (or a 'system' or per-platform form) to the "
                    "package manifest.");
            }
        } else if (libName) {
            bool already = false;
            for (auto& l : linkLibraries_) { if (l == *libName) { already = true; break; } }
            if (!already) linkLibraries_.push_back(*libName);
        }
        for (auto& decl : block.declarations()) {
            auto fname = decl.nameText().value_or(std::u16string{});
            uint32_t fPos = decl.nameToken() ? decl.nameToken()->startOffset() : decl.node.startOffset();
            Type* retType = decl.returnType() && decl.returnType()->typeReference()
                ? resolveTypeReference(*decl.returnType()->typeReference())
                : typeCtx.getPrimitive(TypeKind::Void);
            Symbol* sym = makeSymbol(SymbolKind::Function, fname, nullptr, fPos);
            sym->returnType = retType;
            sym->funcDeclCst = decl.node.greenNode();
            sym->isExternal = true;
            sym->libraryName = libName ? *libName : std::u16string{};

            for (auto& p : decl.parameters()) {
                Type* pt = p.typeReference()
                    ? resolveTypeReference(*p.typeReference())
                    : typeCtx.getError();
                bool isOut = p.isOut();
                if (isOut) {
                    bool ok = false;
                    if (pt && !pt->isError()) {
                        Type* base = pt->isOptional() ? pt->inner : pt;
                        ok = base && (base->isPrimitive() || base->isExternal());
                    }
                    if (!ok) {
                        auto pname = p.nameText().value_or(std::u16string{});
                        errorAtNode(p.node, "Out parameter '" + asciiOf(pname) +
                            "' of '" + asciiOf(fname) + "' has type '" +
                            (pt ? pt->toString() : std::string("<unknown>")) +
                            "'. 'out' parameters must be a primitive or an external type.");
                    }
                }
                // Array params: only primitive or external element types are valid
                // at the C ABI boundary (class/struct elements would require the C
                // side to understand the Ens object header).
                if (pt && !pt->isError()) {
                    Type* arrCheck = pt->isOptional() ? pt->inner : pt;
                    if (arrCheck && arrCheck->isArray()) {
                        if (isOut) {
                            auto pname = p.nameText().value_or(std::u16string{});
                            errorAtNode(p.node, "Out parameter '" + asciiOf(pname) +
                                "' of '" + asciiOf(fname) + "' has type '" + pt->toString() +
                                "'. Array 'out' parameters are not supported.");
                        }
                        Type* elem = arrCheck->inner;
                        bool elemOk = elem && (elem->isPrimitive() || elem->isExternal());
                        if (!elemOk) {
                            auto pname = p.nameText().value_or(std::u16string{});
                            errorAtNode(p.node, "External function parameter '" + asciiOf(pname) +
                                "' of '" + asciiOf(fname) + "' has type '" + pt->toString() +
                                "'. Array elements passed to C must be a primitive or an external type.");
                        }
                    }
                }
                if (p.defaultValue()) {
                    errorAtNode(p.node, "External function parameters cannot have default values.");
                }
                if (p.isThisField()) {
                    errorAtNode(p.node, "'this.' parameters are not allowed in external functions.");
                }
                sym->paramTypes.push_back(pt);
                sym->paramIsOut.push_back(isOut);
            }

            if (!globalScope->define(sym)) {
                Symbol* existing = globalScope->lookupLocal(fname);
                if (existing && existing->kind == SymbolKind::Function) {
                    errorAtNode(decl.node, "External function '" + asciiOf(fname) +
                        "' cannot share its name with another function; C symbols cannot be overloaded.");
                } else {
                    errorAtNode(decl.node, "Duplicate function name '" + asciiOf(fname) + "'");
                }
            }
            analysis.setSymbol(decl.node.greenNode(), sym);
        }
    }
}

void Analyzer::registerStructNames(const ast::SourceFile& file) {
    for (auto& sd : file.structs()) {
        auto name = sd.nameText();
        if (!name) continue;
        if (Type* existing = typeCtx.lookupNamedType(modulePath_, *name)) {
            errorAtNode(sd.node, "Duplicate type '" + asciiOf(*name) +
                "'; this file already declares a " + typeKindWord(existing) + " with this name.");
            continue;
        }
        Type* t = typeCtx.registerStruct(modulePath_, *name);
        auto [line, col] = source.offsetToPosition(
            sd.nameToken() ? sd.nameToken()->startOffset() : sd.node.startOffset());
        t->structInfo->line = line;
        t->structInfo->column = col;
        t->structInfo->visibility = topLevelVisibility(sd.visibilityModifier(), asciiOf(*name));
        t->structInfo->packagePrefix = packagePrefix_;
        for (auto& tp : sd.typeParams()) {
            t->structInfo->isTemplate = true;
            t->structInfo->typeParamNames.push_back(tp.nameText().value_or(std::u16string{}));
        }
        analysis.setType(sd.node.greenNode(), t);
    }
}

void Analyzer::registerClassNames(const ast::SourceFile& file) {
    for (auto& cd : file.classes()) {
        auto name = cd.nameText();
        if (!name) continue;
        if (Type* existing = typeCtx.lookupNamedType(modulePath_, *name)) {
            errorAtNode(cd.node, "Duplicate type '" + asciiOf(*name) +
                "'; this file already declares a " + typeKindWord(existing) + " with this name.");
            continue;
        }
        Type* t = typeCtx.registerClass(modulePath_, *name);
        auto [line, col] = source.offsetToPosition(
            cd.nameToken() ? cd.nameToken()->startOffset() : cd.node.startOffset());
        t->structInfo->line = line;
        t->structInfo->column = col;
        t->structInfo->visibility = topLevelVisibility(cd.visibilityModifier(), asciiOf(*name));
        t->structInfo->packagePrefix = packagePrefix_;
        t->structInfo->isAbstract = cd.isAbstract();
        t->structInfo->isFinal = cd.isFinal();
        t->structInfo->isSealed = cd.isSealed();
        for (auto& tp : cd.typeParams()) {
            t->structInfo->isTemplate = true;
            t->structInfo->typeParamNames.push_back(tp.nameText().value_or(std::u16string{}));
        }
        analysis.setType(cd.node.greenNode(), t);
    }
}

void Analyzer::registerInterfaceNames(const ast::SourceFile& file) {
    for (auto& id : file.interfaces()) {
        auto name = id.nameText();
        if (!name) continue;
        if (Type* existing = typeCtx.lookupNamedType(modulePath_, *name)) {
            errorAtNode(id.node, "Duplicate type '" + asciiOf(*name) +
                "'; this file already declares a " + typeKindWord(existing) + " with this name.");
            continue;
        }
        Type* t = typeCtx.registerInterface(modulePath_, *name);
        auto [line, col] = source.offsetToPosition(
            id.nameToken() ? id.nameToken()->startOffset() : id.node.startOffset());
        t->structInfo->line = line;
        t->structInfo->column = col;
        t->structInfo->visibility = topLevelVisibility(id.visibilityModifier(), asciiOf(*name));
        t->structInfo->packagePrefix = packagePrefix_;
        for (auto& tp : id.typeParams()) {
            t->structInfo->isTemplate = true;
            t->structInfo->typeParamNames.push_back(tp.nameText().value_or(std::u16string{}));
        }
        analysis.setType(id.node.greenNode(), t);
    }
}

void Analyzer::registerEnumNames(const ast::SourceFile& file) {
    for (auto& ed : file.enums()) {
        auto name = ed.nameText();
        if (!name) continue;
        if (Type* existing = typeCtx.lookupNamedType(modulePath_, *name)) {
            errorAtNode(ed.node, "Duplicate type '" + asciiOf(*name) +
                "'; this file already declares a " + typeKindWord(existing) + " with this name.");
            continue;
        }
        Type* t = typeCtx.registerEnum(modulePath_, *name);
        auto [line, col] = source.offsetToPosition(
            ed.nameToken() ? ed.nameToken()->startOffset() : ed.node.startOffset());
        t->structInfo->line = line;
        t->structInfo->column = col;
        t->structInfo->visibility = topLevelVisibility(ed.visibilityModifier(), asciiOf(*name));
        t->structInfo->packagePrefix = packagePrefix_;
        analysis.setType(ed.node.greenNode(), t);
    }
}

void Analyzer::collectEnums(const ast::SourceFile& file) {
    for (auto& ed : file.enums()) {
        Type* t = analysis.typeOf(ed.node.greenNode());
        if (!t || !t->structInfo) continue;
        StructInfo* si = t->structInfo;
        auto members = ed.members();

        // An enum is numeric when any member carries an explicit `= value`; a
        // plain enum keeps its members' declaration ordinals as their values.
        bool numeric = false;
        for (auto& m : members) {
            if (m.value()) { numeric = true; break; }
        }
        si->enumIsNumeric = numeric;

        int64_t next = 0;
        bool isFirst = true;
        for (auto& m : members) {
            auto mname = m.nameText();
            if (!mname) continue;
            bool dupName = false;
            for (auto& existing : si->enumMembers) {
                if (existing.name == *mname) { dupName = true; break; }
            }
            if (dupName) {
                errorAtNode(m.node, "Enum '" + asciiOf(si->name) + "' already has a member named '" +
                    asciiOf(*mname) + "'; remove the duplicate.");
                continue;
            }

            int64_t value = next;
            if (auto dv = m.value()) {
                auto expr = dv->expression();
                std::optional<int64_t> v = expr ? integerConstantValue(*expr) : std::nullopt;
                if (v) {
                    value = *v;
                } else {
                    errorAtNode(expr ? expr->node : m.node, "The value of enum member '" +
                        asciiOf(*mname) + "' must be an integer constant, for example '" +
                        asciiOf(*mname) + " = 1'.");
                }
            } else if (numeric && isFirst) {
                errorAtNode(m.node, "Enum '" + asciiOf(si->name) + "' assigns values to its "
                    "members, so its first member '" + asciiOf(*mname) + "' must have one too; "
                    "give it a value, for example '" + asciiOf(*mname) + " = 0'.");
            }
            next = value + 1;
            isFirst = false;

            if (numeric) {
                for (auto& existing : si->enumMembers) {
                    if (existing.value == value) {
                        errorAtNode(m.node, "Enum members '" + asciiOf(*mname) + "' and '" +
                            asciiOf(existing.name) + "' both have the value " +
                            std::to_string(value) + "; give each member of enum '" +
                            asciiOf(si->name) + "' a distinct value.");
                        break;
                    }
                }
            }

            EnumMemberInfo info;
            info.name = *mname;
            info.value = value;
            si->enumMembers.push_back(std::move(info));
        }
        if (si->enumMembers.empty()) {
            errorAtNode(ed.node, "Enum '" + asciiOf(si->name) +
                "' must have at least one member.");
        }
    }
}

std::u16string Analyzer::importTargetPath(const ast::ImportDecl& imp) const {
    std::u16string mp = imp.modulePath();
    if (imp.isPackage() || packagePrefix_.empty()) return mp;
    return packagePrefix_ + u"." + mp;
}

void Analyzer::bindImports(const ModuleResolver& resolver) {
    bindTypeImports(resolver);
    bindValueImports(resolver);
}

// Bind imported types and namespace aliases. Runs before signatures are resolved so
// a signature can reference an imported type. Named imports that do not resolve to a
// type are left for bindValueImports (functions are not collected yet).
void Analyzer::bindTypeImports(const ModuleResolver& resolver) {
    if (!astRoot) return;
    for (auto& imp : astRoot->imports()) {
        std::u16string targetPath = importTargetPath(imp);
        const Analyzer* target = resolver(targetPath);
        if (!target) {
            // A module that failed to load already produced a clear diagnostic in the
            // module-graph phase (unknown package, missing file, missing '@'); stay quiet.
            continue;
        }

        if (auto alias = imp.aliasText()) {
            // Named import: `import Alias from path;`, bring `Alias` into scope.
            Type* importedType = typeCtx.lookupNamedType(targetPath, *alias);
            if (importedType && !isTypeVisibleFrom(importedType)) {
                errorAtNode(imp.node, invisibleTypeMessage(*alias, importedType));
                continue;
            }
            if (importedType) {
                uint32_t namePos = imp.aliasToken() ? imp.aliasToken()->startOffset() : imp.node.startOffset();
                Symbol* sym = makeSymbol(SymbolKind::Variable, *alias, importedType, namePos);
                sym->isTypeName = true;
                analysis.setSymbol(imp.node.greenNode(), sym);
                if (!globalScope->define(sym)) {
                    errorAtNode(imp.node, "Imported name '" + asciiOf(*alias) +
                        "' conflicts with an existing declaration");
                }
            }
        } else {
            // Namespace import: `import path;`, last path segment becomes the alias.
            auto nsName = imp.namespaceName();
            if (!nsName) continue;
            Symbol* sym = makeSymbol(SymbolKind::Namespace, *nsName, nullptr, imp.node.startOffset());
            sym->namespaceModulePath = targetPath;
            sym->namespaceTarget = target;
            if (!globalScope->define(sym)) {
                errorAtNode(imp.node, "Namespace alias '" + asciiOf(*nsName) +
                    "' conflicts with an existing declaration");
            }
        }
    }
}

// Diagnose named imports that resolve to a free function. Runs after signatures are
// resolved so the target module's functions exist in its global scope. Type imports are
// already bound in bindTypeImports; functions may only be used namespace-qualified.
void Analyzer::bindValueImports(const ModuleResolver& resolver) {
    if (!astRoot) return;
    for (auto& imp : astRoot->imports()) {
        auto alias = imp.aliasText();
        if (!alias) continue;            // namespace imports bound in bindTypeImports
        std::u16string targetPath = importTargetPath(imp);
        const Analyzer* target = resolver(targetPath);
        if (!target) continue;           // unresolved import already diagnosed in the module graph
        if (typeCtx.lookupNamedType(targetPath, *alias)) continue;  // bound as a type in bindTypeImports

        Symbol* fnSym = target->globalSymbol(*alias);
        if (fnSym && fnSym->kind == SymbolKind::Function) {
            auto segs = imp.pathSegments();
            std::u16string ns = segs.empty() ? std::u16string() : segs.back();
            std::string moduleImport = (imp.isPackage() ? "@" : "") + asciiOf(targetPath);
            errorAtNode(imp.node, "Function '" + asciiOf(*alias) +
                "' cannot be imported by name. Import the module instead: 'import " +
                moduleImport + ";' then call it as '" + asciiOf(ns) + "." + asciiOf(*alias) + "'.");
            continue;
        }
        errorAtNode(imp.node, "Module '" + asciiOf(targetPath) +
            "' has no top-level declaration named '" + asciiOf(*alias) + "'.");
    }
}

void Analyzer::analyzeBodies() {
    if (!astRoot) return;
    auto& sf = *astRoot;

    auto runChecks = [&](const ast::FuncDecl& fn) {
        checkParameterDefaults(fn);
    };
    for (auto& fn : sf.functions()) runChecks(fn);
    for (auto& sd : sf.structs()) for (auto& m : sd.methods()) runChecks(m);
    for (auto& cd : sf.classes()) for (auto& m : cd.methods()) runChecks(m);

    for (auto& sd : sf.structs()) checkFieldDefaults(sd);
    for (auto& cd : sf.classes()) checkFieldDefaults(cd);
    for (auto& sd : sf.structs()) checkFieldInitialization(sd);
    for (auto& cd : sf.classes()) checkFieldInitialization(cd);

    auto resolveThrows = [&](const ast::FuncDecl& fn) {
        auto* info = analysis.find(fn.node.greenNode());
        if (info && info->resolvedSymbol) resolveDeclaredThrows(fn, info->resolvedSymbol);
    };
    for (auto& fn : sf.functions()) resolveThrows(fn);
    for (auto& sd : sf.structs()) for (auto& m : sd.methods()) resolveThrows(m);
    for (auto& cd : sf.classes()) for (auto& m : cd.methods()) resolveThrows(m);
    for (auto& id : sf.interfaces()) for (auto& m : id.methods()) resolveThrows(m);

    // A test's implicit declared contract is `throws Error`.
    for (auto& td : sf.tests()) {
        auto* info = analysis.find(td.node.greenNode());
        if (info && info->resolvedSymbol && errorClassInfo_ &&
            info->resolvedSymbol->declaredThrowsTypes.empty()) {
            info->resolvedSymbol->declaredThrowsTypes.push_back(errorClassInfo_);
        }
    }

    for (auto& fn : sf.functions()) analyzeFunctionBody(fn);
    for (auto& sd : sf.structs())   for (auto& m : sd.methods()) analyzeFunctionBody(m);
    for (auto& cd : sf.classes())   for (auto& m : cd.methods()) analyzeFunctionBody(m);
    for (auto& td : sf.tests())     analyzeTestBody(td);
}

void Analyzer::resolveDeclaredThrows(const ast::FuncDecl& fn, Symbol* sym) {
    for (auto& tr : fn.declaredThrowsTypes()) {
        Type* t = resolveTypeReference(tr);
        if (t->isError()) continue;
        bool isErrorSubclass = t->isClass() && t->structInfo && errorClassInfo_ &&
            t->structInfo->isSubclassOf(errorClassInfo_);
        if (!isErrorSubclass) {
            errorAtNode(tr.node, "A declared throws type must be 'Error' or a subclass; '" +
                t->toString() + "' is not.");
            continue;
        }
        StructInfo* si = t->structInfo;
        bool redundant = false;
        for (StructInfo* existing : sym->declaredThrowsTypes) {
            if (si == existing || si->isSubclassOf(existing) || existing->isSubclassOf(si)) {
                errorAtNode(tr.node, "Declared throws type '" + asciiOf(si->name) +
                    "' overlaps with another type in the list; list each exception once.");
                redundant = true;
                break;
            }
        }
        if (!redundant) sym->declaredThrowsTypes.push_back(si);
    }
}

// =========================================================
// Collect phase
// =========================================================

// Flag every method whose name is declared more than once in the same owner so
// codegen mangles each overload distinctly.
static void markOverloadedMethods(StructInfo* si) {
    if (!si) return;
    for (auto& mi : si->methods) {
        if (!mi.symbol) continue;
        for (auto& other : si->methods) {
            if (&other != &mi && other.name == mi.name) {
                mi.symbol->isOverloaded = true;
                break;
            }
        }
    }
}

// Same-name methods callable on `si`, most-derived first. A base declaration
// with the same signature as a more derived one is shadowed (an overridden or
// hidden method) and is not a separate candidate. Two same-signature methods in
// one class (overloads collapsed by a generic instantiation) both stay, so a
// call to them is reported as ambiguous.
static std::vector<const MethodInfo*> collectMethodCandidates(StructInfo* si,
                                                              const std::u16string& name) {
    std::vector<const MethodInfo*> out;
    for (StructInfo* s = si; s; s = s->baseInfo) {
        size_t derivedCount = out.size();
        for (const auto& mi : s->methods) {
            if (mi.name != name || !mi.symbol) continue;
            bool shadowed = false;
            for (size_t i = 0; i < derivedCount; ++i) {
                if (sameParameterTypes(out[i]->symbol, mi.symbol)) { shadowed = true; break; }
            }
            if (!shadowed) out.push_back(&mi);
        }
    }
    return out;
}

static bool callUsesNamedArguments(const std::vector<ast::Expression>& args) {
    for (auto& a : args) {
        if (a.asNamedArgument()) return true;
    }
    return false;
}

static std::string signatureOf(Symbol* sym) {
    std::string sig = asciiOf(sym->name) + "(";
    for (size_t i = 0; i < sym->paramTypes.size(); ++i) {
        if (i) sig += ", ";
        sig += sym->paramTypes[i] ? sym->paramTypes[i]->toString() : "?";
    }
    sig += ")";
    return sig;
}

// Full display signature of an interface method, including the return type.
static std::string interfaceMethodSignature(const MethodInfo& mi) {
    std::string sig = signatureOf(mi.symbol);
    if (mi.symbol && mi.symbol->returnType && !mi.symbol->returnType->isVoid()) {
        sig += " -> " + mi.symbol->returnType->toString();
    }
    return sig;
}

void Analyzer::collectStructs(const ast::SourceFile& file) {
    auto structs = file.structs();

    for (auto& sd : structs) {
        Type* t = analysis.typeOf(sd.node.greenNode());
        if (!t) continue;
        size_t tpCount = t->structInfo ? enterTemplateScope(t->structInfo, sd.typeParams()) : 0;
        for (auto& f : sd.fields()) {
            FieldInfo fi;
            auto fname = f.nameText();
            if (fname) fi.name = *fname;
            Type* ft = f.typeReference() ? resolveTypeReference(*f.typeReference()) : typeCtx.getError();
            fi.type = ft;
            fi.visibility = memberVisibility(f.visibilityModifier(),
                                             followedMemberVisibility(t->structInfo->visibility),
                                             t->structInfo, "Field", fi.name);
            fi.isWeak = f.isWeak();
            if (fi.isWeak) {
                errorAtNode(f.node, "'weak' fields are not allowed on structs");
            }
            auto [line, col] = source.offsetToPosition(
                f.nameToken() ? f.nameToken()->startOffset() : f.node.startOffset());
            fi.line = line;
            fi.column = col;
            fi.declaration = f.node.greenNode();
            fi.definingClass = t->structInfo;
            t->structInfo->fields.push_back(std::move(fi));
        }
        popTypeParams(tpCount);
    }

    for (auto& sd : structs) {
        Type* t = analysis.typeOf(sd.node.greenNode());
        if (!t) continue;
        size_t tpCount = t->structInfo ? enterTemplateScope(t->structInfo, sd.typeParams()) : 0;
        for (auto& m : sd.methods()) {
            bool isCtor = m.isConstructor();
            if (m.isDestructor()) {
                errorAtNode(m.node, "A struct cannot declare a destructor; destructors are "
                    "only allowed on classes.");
                continue;
            }
            auto rawName = m.nameText().value_or(std::u16string{});
            if (!isCtor && !rawName.empty() && rawName == t->structInfo->name) {
                errorAtNode(m.node, "To declare a constructor, use the 'constructor' keyword. "
                    "A method cannot be named after its struct '" +
                    asciiOf(t->structInfo->name) + "'.");
                continue;
            }
            std::u16string mname = isCtor ? std::u16string(u"constructor") : rawName;

            Type* retType = m.returnType() && m.returnType()->typeReference()
                ? resolveTypeReference(*m.returnType()->typeReference())
                : typeCtx.getPrimitive(TypeKind::Void);
            uint32_t mPos = m.nameToken() ? m.nameToken()->startOffset() : m.node.startOffset();
            Symbol* sym = makeSymbol(SymbolKind::Function, mname, nullptr, mPos);
            sym->returnType = retType;
            sym->funcDeclCst = m.node.greenNode();
            sym->declaredThrows = m.isThrows();
            sym->abiThrows = m.isThrows();  // structs have no inheritance
            sym->methodOwner = t->structInfo;
            sym->isConstructor = isCtor;
            sym->isNoreturn = m.isNoreturn();
            checkFieldMethodCollision(t->structInfo, mname, isCtor, m.node);
            checkThrowsClausePlacement(m, /*isOverridable=*/false, /*isConstructor=*/isCtor);
            checkNoreturnPlacement(m, /*isConstructor=*/isCtor, /*isDestructor=*/false);
            resolveMethodParams(m, t, sym);
            checkHashMethodSignature(m, sym, isCtor);
            analysis.setSymbol(m.node.greenNode(), sym);
            analysis.setReceiver(m.node.greenNode(), t);

            if (t->structInfo->findMethodIndexBySignature(mname, sym) >= 0) {
                errorAtNode(m.node, "Method '" + asciiOf(mname) + "' of '" +
                    asciiOf(t->structInfo->name) + "' is already declared with the same "
                    "parameter types; overloads must differ in parameter count or types.");
                continue;
            }

            MethodInfo mi;
            mi.name = mname;
            mi.symbol = sym;
            mi.declaration = const_cast<GreenElement*>(m.node.greenNode());
            mi.visibility = memberVisibility(m.visibilityModifier(), Visibility::Private,
                                             t->structInfo,
                                             isCtor ? "Constructor" : "Method", mname);
            mi.isConstructor = isCtor;
            mi.isNoreturn = m.isNoreturn();
            mi.definingClass = t->structInfo;
            t->structInfo->methods.push_back(std::move(mi));
        }
        markOverloadedMethods(t->structInfo);
        t->structInfo->membersCollected = true;
        popTypeParams(tpCount);
    }
}

void Analyzer::collectInterfaces(const ast::SourceFile& file) {
    for (auto& id : file.interfaces()) {
        Type* t = analysis.typeOf(id.node.greenNode());
        if (!t || !t->structInfo) continue;
        StructInfo* si = t->structInfo;
        size_t tpCount = enterTemplateScope(si, id.typeParams());

        for (auto& f : id.fields()) {
            errorAtNode(f.node, "An interface cannot declare fields; its body lists only "
                "method signatures.");
        }

        for (auto& m : id.methods()) {
            if (m.isConstructor()) {
                errorAtNode(m.node, "An interface cannot declare a constructor; '" +
                    asciiOf(si->name) + "' has no instances of its own.");
                continue;
            }
            if (m.isDestructor()) {
                errorAtNode(m.node, "An interface cannot declare a destructor; '" +
                    asciiOf(si->name) + "' has no instances of its own.");
                continue;
            }
            auto mname = m.nameText().value_or(std::u16string{});
            if (!mname.empty() && mname == si->name) {
                errorAtNode(m.node, "An interface cannot declare a constructor; '" +
                    asciiOf(si->name) + "' has no instances of its own.");
                continue;
            }
            if (auto modifier = m.visibilityModifier()) {
                errorAtNode(modifier->node, "The members of an interface always share the "
                    "interface's visibility; remove the modifier from '" + asciiOf(mname) + "'.");
            }
            if (m.isOverride() || m.isFinal() || m.isAbstract()) {
                errorAtNode(m.node, "Interface methods are plain signatures; 'abstract', "
                    "'override', and 'final' are not allowed on '" + asciiOf(mname) + "'.");
            }
            if (m.body().has_value()) {
                errorAtNode(m.node, "Interface method '" + asciiOf(mname) + "' cannot have a "
                    "body; end the signature with ';'. Implementing classes provide the body.");
            }
            if (m.isThrows() && m.declaredThrowsTypes().empty()) {
                errorAtNode(m.throwsToken().value_or(m.node), "An interface method marked "
                    "'throws' must list its exception types, e.g. 'throws IOError'.");
            }

            Type* retType = m.returnType() && m.returnType()->typeReference()
                ? resolveTypeReference(*m.returnType()->typeReference())
                : typeCtx.getPrimitive(TypeKind::Void);
            uint32_t mPos = m.nameToken() ? m.nameToken()->startOffset() : m.node.startOffset();
            Symbol* sym = makeSymbol(SymbolKind::Function, mname, nullptr, mPos);
            sym->returnType = retType;
            sym->funcDeclCst = m.node.greenNode();
            sym->declaredThrows = m.isThrows();
            sym->abiThrows = m.isThrows();
            sym->methodOwner = si;
            sym->isNoreturn = m.isNoreturn();
            resolveMethodParams(m, t, sym, /*isInterfaceMethod=*/true);
            checkHashMethodSignature(m, sym, /*isConstructor=*/false);
            checkNoreturnPlacement(m, /*isConstructor=*/false, /*isDestructor=*/false);
            analysis.setSymbol(m.node.greenNode(), sym);

            if (si->findMethodIndexBySignature(mname, sym) >= 0) {
                errorAtNode(m.node, "Method '" + asciiOf(mname) + "' of '" + asciiOf(si->name) +
                    "' is already declared with the same parameter types; overloads must "
                    "differ in parameter count or types.");
                continue;
            }

            MethodInfo mi;
            mi.name = mname;
            mi.symbol = sym;
            mi.declaration = const_cast<GreenElement*>(m.node.greenNode());
            mi.visibility = followedMemberVisibility(si->visibility);
            mi.isAbstract = true;  // no body; never emitted as a function
            mi.isNoreturn = m.isNoreturn();
            mi.itableSlot = static_cast<int>(si->methods.size());
            mi.definingClass = si;
            si->methods.push_back(std::move(mi));
        }
        markOverloadedMethods(si);
        si->membersCollected = true;
        popTypeParams(tpCount);
    }
}

// Chain depth for base-before-derived ordering. A base that is a generic
// instantiation may not be filled yet, so its chain continues through the
// template. The seen set guards against not-yet-reported inheritance cycles.
static int baseDepth(StructInfo* si) {
    int d = 0;
    std::unordered_set<StructInfo*> seen;
    for (StructInfo* s = si->baseInfo; s && seen.insert(s).second; ) {
        ++d;
        StructInfo* authority = s->templateOf ? s->templateOf : s;
        s = authority->baseInfo;
    }
    return d;
}

// Direct subclasses are recorded on the declared class: extending an
// instantiation of a generic base records the subclass on its template.
static StructInfo* baseSubclassAuthority(StructInfo* base) {
    return base->templateOf ? base->templateOf : base;
}

void Analyzer::resolveClassBases(const ast::SourceFile& file) {
    auto classes = file.classes();

    // --- Pass A: record class modifiers and resolve base links. ---
    for (auto& cd : classes) {
        Type* t = analysis.typeOf(cd.node.greenNode());
        if (!t || !t->structInfo) continue;
        StructInfo* si = t->structInfo;
        if (si->isSealed && si->isFinal) {
            errorAtNode(cd.node, "Class '" + asciiOf(si->name) + "' cannot be both 'sealed' and "
                "'final'; 'final' already forbids subclasses, so there is nothing to seal.");
        }
        if (auto baseName = cd.baseClassName()) {
            SyntaxNode diag = cd.baseClassToken().value_or(cd.node);
            Type* baseT = typeCtx.lookupNamedType(modulePath_, *baseName);
            if (!baseT && globalScope) {
                if (Symbol* sym = globalScope->lookupLocal(*baseName)) {
                    if (sym->type && sym->type->isClass()) baseT = sym->type;
                }
            }
            if (!baseT) {
                errorAtNode(diag, "Unknown base class '" + asciiOf(*baseName) + "'");
                continue;
            }
            if (!baseT->isClass() || !baseT->structInfo) {
                errorAtNode(diag, "'" + asciiOf(*baseName) +
                    "' is not a class; only classes can be extended");
                continue;
            }
            if (baseT->structInfo->isInterface) {
                errorAtNode(diag, "Cannot extend interface '" + asciiOf(*baseName) +
                    "'; a class implements an interface with 'implements " +
                    asciiOf(*baseName) + "'.");
                continue;
            }
            if (baseT->structInfo == si) {
                errorAtNode(diag, "Class '" + asciiOf(si->name) + "' cannot extend itself");
                continue;
            }
            if (auto baseTok = cd.baseClassToken()) {
                analysis.setType(baseTok->greenNode(), baseT);
            }
            auto baseArgs = cd.baseTypeArguments();
            if (baseT->structInfo->isTemplate) {
                // A generic base must be extended as a full instantiation whose
                // type arguments may use the subclass's own type parameters.
                size_t arity = baseT->structInfo->typeParamNames.size();
                if (baseArgs.size() != arity) {
                    errorAtNode(diag, "Generic base class '" + asciiOf(*baseName) + "' expects " +
                        std::to_string(arity) + (arity == 1 ? " type argument" : " type arguments") +
                        ", but " + std::to_string(baseArgs.size()) +
                        (baseArgs.size() == 1 ? " was given" : " were given"));
                    continue;
                }
                size_t tpCount = enterTemplateScope(si, cd.typeParams());
                std::vector<Type*> argTypes;
                bool ok = true;
                static const std::vector<StructInfo*> kNoBounds;
                for (size_t i = 0; i < baseArgs.size(); ++i) {
                    Type* at = resolveTypeReference(baseArgs[i]);
                    if (at->isError()) ok = false;
                    const std::vector<StructInfo*>& bounds =
                        i < baseT->structInfo->typeParamBounds.size()
                            ? baseT->structInfo->typeParamBounds[i] : kNoBounds;
                    std::u16string pname = i < baseT->structInfo->typeParamNames.size()
                        ? baseT->structInfo->typeParamNames[i] : std::u16string{};
                    if (ok && !checkTypeArgBound(at, bounds, pname, baseArgs[i].node)) ok = false;
                    argTypes.push_back(at);
                }
                popTypeParams(tpCount);
                if (!ok) continue;
                Type* baseInst = typeCtx.instantiate(baseT, argTypes);
                if (baseInst->isError() || !baseInst->structInfo) continue;
                baseT = baseInst;
            } else if (!baseArgs.empty()) {
                errorAtNode(diag, "'" + asciiOf(*baseName) +
                    "' is not generic and takes no type arguments");
                continue;
            }
            if (baseT->structInfo->templateOf == si) {
                errorAtNode(diag, "Class '" + asciiOf(si->name) + "' cannot extend itself");
                continue;
            }
            if (baseT->structInfo->isFinal) {
                errorAtNode(diag, "Cannot extend '" + asciiOf(*baseName) +
                    "' because it is declared 'final'");
                continue;
            }
            if (baseT->structInfo->isSealed && baseT->structInfo->modulePath != modulePath_) {
                errorAtNode(diag, "Cannot extend '" + asciiOf(*baseName) + "' because it is "
                    "declared 'sealed'; every subclass of a sealed class must be declared in "
                    "the module that declares it.");
                continue;
            }
            si->baseInfo = baseT->structInfo;
            baseSubclassAuthority(si->baseInfo)->directSubclasses.push_back(si);
        }

        // --- Resolve the implements clause. ---
        auto ifaceRefs = cd.implementedInterfaceRefs();
        if (!ifaceRefs.empty()) {
            size_t tpCount = enterTemplateScope(si, cd.typeParams());
            for (auto& tr : ifaceRefs) {
                Type* it = resolveTypeReference(tr);
                if (it->isError()) continue;
                if (it->isOptional() || it->isArray()) {
                    errorAtNode(tr.node, "'implements' takes plain interface names; '" +
                        it->toString() + "' is not an interface type.");
                    continue;
                }
                if (!it->isInterface()) {
                    if (it->isClass()) {
                        errorAtNode(tr.node, "'" + it->toString() + "' is a class, not an "
                            "interface; use 'extends " + it->toString() + "' for a base class.");
                    } else {
                        errorAtNode(tr.node, "'" + it->toString() + "' is not an interface; "
                            "'implements' takes interfaces only.");
                    }
                    continue;
                }
                bool dup = false;
                for (Type* prev : si->implementedInterfaces) {
                    if (prev == it) { dup = true; break; }
                }
                if (dup) {
                    errorAtNode(tr.node, "Interface '" + it->toString() +
                        "' is listed more than once in the 'implements' clause.");
                    continue;
                }
                si->implementedInterfaces.push_back(it);
            }
            popTypeParams(tpCount);
        }
    }

    // --- Detect inheritance cycles; break the offending link. ---
    for (auto& cd : classes) {
        Type* t = analysis.typeOf(cd.node.greenNode());
        if (!t || !t->structInfo) continue;
        StructInfo* si = t->structInfo;
        std::unordered_set<StructInfo*> seen;
        bool cycle = false;
        for (StructInfo* s = si; s && !cycle; ) {
            cycle = !seen.insert(s).second;
            if (cycle) break;
            StructInfo* authority = s->templateOf ? s->templateOf : s;
            if (authority != s) {
                cycle = !seen.insert(authority).second;
                if (cycle) break;
            }
            s = authority->baseInfo;
        }
        if (cycle) {
            errorAtNode(cd.node, "Class '" + asciiOf(si->name) +
                "' eventually extends itself through its base classes. "
                "A class cannot inherit from itself, directly or indirectly.");
            auto& siblings = baseSubclassAuthority(si->baseInfo)->directSubclasses;
            siblings.erase(std::remove(siblings.begin(), siblings.end(), si), siblings.end());
            si->baseInfo = nullptr;
        }
    }
}

bool Analyzer::overrideSignaturesCompatible(const MethodInfo& base, Symbol* der) {
    Symbol* bs = base.symbol;
    if (!bs || !der) return true;
    if (bs->paramTypes.size() != der->paramTypes.size()) return false;
    for (size_t i = 0; i < bs->paramTypes.size(); ++i) {
        if (!bs->paramTypes[i] || !der->paramTypes[i]) continue;
        if (!bs->paramTypes[i]->equals(der->paramTypes[i])) return false;
    }
    Type* br = bs->returnType ? bs->returnType : typeCtx.getPrimitive(TypeKind::Void);
    Type* dr = der->returnType ? der->returnType : typeCtx.getPrimitive(TypeKind::Void);
    return br->assignableFrom(dr);  // covariant return allowed
}

// The reserved contracts a class uses to become a content-matched key:
// `hash() -> long` for bucketing and `equals(C other) -> bool` for the final
// match. A method carrying either name must match its signature exactly.
static bool hashSignatureConforms(const Symbol* sym) {
    return sym && sym->paramTypes.empty() && sym->returnType &&
        sym->returnType->kind == TypeKind::Long;
}

static bool equalsSignatureConforms(const Symbol* sym) {
    if (!sym || sym->paramTypes.size() != 1 || !sym->returnType ||
        sym->returnType->kind != TypeKind::Bool) {
        return false;
    }
    Type* p = sym->paramTypes[0];
    return p && p->isClass() && p->structInfo == sym->methodOwner;
}

void Analyzer::layoutOneClass(const ast::ClassDecl& cd) {
    Type* t = analysis.typeOf(cd.node.greenNode());
    if (!t || !t->structInfo) return;
    StructInfo* si = t->structInfo;
    size_t tpCount = enterTemplateScope(si, cd.typeParams());

    // A generic base is an instantiation created during base resolution; its
    // fill may have been deferred until the base template was laid out.
    if (si->baseInfo && si->baseInfo->templateOf) {
        typeCtx.ensureFilled(si->baseInfo);
    }

    // Fields: flatten inherited fields first (base is already laid out), then own.
    if (si->baseInfo) {
        si->fields = si->baseInfo->fields;
        si->baseFieldCount = static_cast<int>(si->baseInfo->fields.size());
    }
    for (auto& f : cd.fields()) {
        FieldInfo fi;
        auto fname = f.nameText();
        if (fname) fi.name = *fname;
        Type* ft = f.typeReference() ? resolveTypeReference(*f.typeReference()) : typeCtx.getError();
        fi.type = ft;
        fi.visibility = memberVisibility(f.visibilityModifier(), Visibility::Private, si,
                                         "Field", fi.name);
        fi.isWeak = f.isWeak();
        fi.definingClass = si;
        if (fi.isWeak) {
            bool ok = ft && ft->isOptional() && ft->inner && ft->inner->isClass();
            if (ok && ft->inner->isInterface()) {
                errorAtNode(f.node, "'weak' fields must reference a class; interface-typed "
                    "weak fields are not supported. Use the implementing class's type instead.");
            } else if (!ok) {
                errorAtNode(f.node,
                    "'weak' fields must be nullable class types (e.g. `weak Foo? f`)");
            }
        }
        if (!fi.name.empty() && si->findFieldIndex(fi.name) >= 0) {
            int existing = si->findFieldIndex(fi.name);
            bool inherited = existing < si->baseFieldCount;
            errorAtNode(f.node, "Field '" + asciiOf(fi.name) + "' is already declared" +
                (inherited ? " in base class '" + asciiOf(si->baseInfo->name) + "'" : " in '" + asciiOf(si->name) + "'"));
            continue;
        }
        auto [line, col] = source.offsetToPosition(
            f.nameToken() ? f.nameToken()->startOffset() : f.node.startOffset());
        fi.line = line;
        fi.column = col;
        fi.declaration = f.node.greenNode();
        si->fields.push_back(std::move(fi));
    }

    // Interface methods a locally-declared method may implement; used to
    // validate 'override' markers the same way base-class methods are.
    auto interfaceDeclaring = [&](const std::u16string& name, Symbol* sym,
                                  bool bySignature) -> Type* {
        for (Type* ifaceT : si->implementedInterfaces) {
            if (!ifaceT || !ifaceT->structInfo) continue;
            StructInfo* iface = ifaceT->structInfo;
            if (iface->templateOf) typeCtx.ensureFilled(iface);
            int idx = bySignature ? iface->findMethodIndexBySignature(name, sym)
                                  : iface->findMethodIndex(name);
            if (idx >= 0) return ifaceT;
        }
        return nullptr;
    };

    // Methods: collect own methods, then validate override/abstract.
    for (auto& m : cd.methods()) {
        bool isCtor = m.isConstructor();
        bool isDtor = m.isDestructor();
        auto rawName = m.nameText().value_or(std::u16string{});
        if (!isCtor && !isDtor && !rawName.empty() && rawName == si->name) {
            errorAtNode(m.node, "To declare a constructor, use the 'constructor' keyword. "
                "A method cannot be named after its class '" + asciiOf(si->name) + "'.");
            continue;
        }
        if (isDtor && si->findDestructorIndex() >= 0) {
            errorAtNode(m.node, "A class can declare at most one destructor.");
            continue;
        }
        std::u16string mname = isCtor ? std::u16string(u"constructor")
                             : isDtor ? std::u16string(u"destructor")
                             : rawName;

        Type* retType = m.returnType() && m.returnType()->typeReference()
            ? resolveTypeReference(*m.returnType()->typeReference())
            : typeCtx.getPrimitive(TypeKind::Void);
        uint32_t mPos = m.nameToken() ? m.nameToken()->startOffset() : m.node.startOffset();
        Symbol* sym = makeSymbol(SymbolKind::Function, mname, nullptr, mPos);
        sym->returnType = retType;
        sym->funcDeclCst = m.node.greenNode();
        sym->declaredThrows = m.isThrows();
        sym->methodOwner = si;
        sym->isConstructor = isCtor;
        sym->isDestructor = isDtor;
        sym->isNoreturn = m.isNoreturn();
        resolveMethodParams(m, t, sym);
        analysis.setSymbol(m.node.greenNode(), sym);
        analysis.setReceiver(m.node.greenNode(), t);

        if (si->findMethodIndexBySignature(mname, sym) >= 0) {
            errorAtNode(m.node, "Method '" + asciiOf(mname) + "' of '" + asciiOf(si->name) +
                "' is already declared with the same parameter types; overloads must "
                "differ in parameter count or types.");
            continue;
        }

        MethodInfo mi;
        mi.name = mname;
        mi.symbol = sym;
        mi.declaration = const_cast<GreenElement*>(m.node.greenNode());
        mi.visibility = isDtor ? Visibility::Private
                               : memberVisibility(m.visibilityModifier(), Visibility::Private,
                                                  si, isCtor ? "Constructor" : "Method", mname);
        mi.isConstructor = isCtor;
        mi.isDestructor = isDtor;
        mi.isOverride = m.isOverride();
        mi.isFinal = m.isFinal();
        mi.isAbstract = m.isAbstract();
        mi.isNoreturn = m.isNoreturn();
        mi.definingClass = si;
        si->methods.push_back(std::move(mi));

        checkNoreturnPlacement(m, /*isConstructor=*/isCtor, /*isDestructor=*/isDtor);

        // Validate (base methods already collected via base-before-derived order).
        if (isDtor) {
            if (m.visibilityModifier() || m.isOverride() || m.isFinal() || m.isAbstract())
                errorAtNode(m.node, "A destructor cannot have modifiers such as visibility, "
                    "'override', 'final', or 'abstract'.");
            if (!m.parameters().empty())
                errorAtNode(m.node, "A destructor cannot declare parameters.");
            if (m.returnType())
                errorAtNode(m.node, "A destructor cannot declare a return type.");
            if (m.isThrows())
                errorAtNode(m.throwsToken().value_or(m.node),
                    "A destructor cannot be marked 'throws'.");
            continue;
        }
        bool overridable = !isCtor && !m.isFinal() && !si->isFinal;
        checkFieldMethodCollision(si, mname, isCtor, m.node);
        checkThrowsClausePlacement(m, overridable, isCtor);
        checkHashMethodSignature(m, sym, isCtor);
        checkEqualsMethodSignature(m, sym, isCtor);
        if (isCtor) {
            if (m.isOverride() || m.isAbstract())
                errorAtNode(m.node, "A constructor cannot be 'override' or 'abstract'");
            continue;
        }
        StructInfo* baseByName = si->baseInfo ? si->baseInfo->classDeclaringMethod(mname) : nullptr;
        StructInfo* baseBySig = si->baseInfo
            ? si->baseInfo->classDeclaringMethodBySignature(mname, sym) : nullptr;
        if (m.isAbstract()) {
            if (!si->isAbstract)
                errorAtNode(m.node, "Abstract method '" + asciiOf(mname) + "' requires class '" +
                    asciiOf(si->name) + "' to be declared 'abstract'");
            if (m.body().has_value())
                errorAtNode(m.node, "Abstract method '" + asciiOf(mname) + "' cannot have a body");
        }
        // A conforming `hash` or `equals` overrides the compiler's built-in
        // identity hash/equality, so it is written with 'override' like any
        // other override. `reservedIntent` also covers the near-miss shapes,
        // which get their own signature diagnostics rather than a spurious
        // "nothing to override".
        bool reservedIntent = mname == u"hash" ||
            (mname == u"equals" && sym->paramTypes.size() == 1 && sym->paramTypes[0] &&
             sym->paramTypes[0]->isClass() && sym->paramTypes[0]->structInfo == sym->methodOwner);
        bool reservedConforming = (mname == u"hash" && hashSignatureConforms(sym)) ||
                                  (mname == u"equals" && equalsSignatureConforms(sym));
        if (m.isOverride()) {
            if (baseBySig) {
                MethodInfo& bm = baseBySig->methods[baseBySig->findMethodIndexBySignature(mname, sym)];
                if (bm.isFinal)
                    errorAtNode(m.node, "Cannot override '" + asciiOf(mname) +
                        "' because it is declared 'final' in '" + asciiOf(baseBySig->name) + "'");
                if (!overrideSignaturesCompatible(bm, sym))
                    errorAtNode(m.node, "Override of '" + asciiOf(mname) +
                        "' does not match the signature declared in '" + asciiOf(baseBySig->name) +
                        "'; expected '" + interfaceMethodSignature(bm) + "'.");
                if (m.isThrows() && bm.symbol && !bm.symbol->declaredThrows)
                    errorAtNode(m.throwsToken().value_or(m.node), "Method '" + asciiOf(mname) +
                        "' is marked 'throws' but overrides a method of '" + asciiOf(baseBySig->name) +
                        "' that is not. Mark the base method 'throws' too, or handle the exceptions "
                        "inside the override.");
                if (bm.isNoreturn && !m.isNoreturn())
                    errorAtNode(m.node, "Override of '" + asciiOf(mname) + "' must be marked "
                        "'noreturn' because the method it overrides in '" + asciiOf(baseBySig->name) +
                        "' is 'noreturn'; callers rely on it never returning. Add 'noreturn' "
                        "before the method name.");
            } else if (interfaceDeclaring(mname, sym, /*bySignature=*/true)) {
                // Implements an interface method; the implements clause check
                // below validates the return type and throws conformance.
            } else if (baseByName) {
                const MethodInfo& bnm = baseByName->methods[baseByName->findMethodIndex(mname)];
                errorAtNode(m.node, "Override of '" + asciiOf(mname) +
                    "' does not match the signature declared in '" + asciiOf(baseByName->name) +
                    "'; expected '" + interfaceMethodSignature(bnm) + "'.");
            } else if (Type* ifaceByName = interfaceDeclaring(mname, sym, /*bySignature=*/false)) {
                StructInfo* ifaceInfo = ifaceByName->structInfo;
                const MethodInfo& ifm = ifaceInfo->methods[ifaceInfo->findMethodIndex(mname)];
                errorAtNode(m.node, "Override of '" + asciiOf(mname) +
                    "' does not match the signature declared in interface '" +
                    ifaceByName->toString() + "'; expected '" + interfaceMethodSignature(ifm) + "'.");
            } else if (reservedIntent) {
                // Overrides the compiler's built-in identity hash/equality; no
                // base declares it, but 'override' is the required marker.
            } else {
                errorAtNode(m.node, "Method '" + asciiOf(mname) +
                    "' is marked 'override' but no base class or implemented interface declares it");
            }
        } else if (baseBySig) {
            // A subclass may add a new overload of an inherited name; only a
            // same-signature redeclaration hides the base method.
            errorAtNode(m.node, "Method '" + asciiOf(mname) + "' hides a method inherited from '" +
                asciiOf(baseBySig->name) + "'; mark it 'override' to replace it, or rename it");
        } else if (Type* ifaceT = interfaceDeclaring(mname, sym, /*bySignature=*/true)) {
            errorAtNode(m.node, "Method '" + asciiOf(mname) + "' of '" + asciiOf(si->name) +
                "' implements a method declared in interface '" + ifaceT->toString() +
                "'; mark it 'override'");
        } else if (reservedConforming) {
            errorAtNode(m.node, "Method '" + asciiOf(mname) + "' overrides the built-in " +
                (mname == u"hash" ? std::string("identity hash") : std::string("identity equality")) +
                "; mark it 'override'.");
        }
    }
    markOverloadedMethods(si);
    checkHashEqualsPairing(cd, si);

    // --- The class must provide every method of every interface it implements,
    // either declared here (abstract counts) or inherited from a base class. ---
    for (Type* ifaceT : si->implementedInterfaces) {
        if (!ifaceT || !ifaceT->structInfo) continue;
        StructInfo* iface = ifaceT->structInfo;
        if (iface->templateOf) typeCtx.ensureFilled(iface);
        for (auto& im : iface->methods) {
            if (!im.symbol) continue;
            StructInfo* decl = si->classDeclaringMethodBySignature(im.name, im.symbol);
            if (!decl) {
                errorAtNode(cd.node, "Class '" + asciiOf(si->name) + "' implements '" +
                    ifaceT->toString() + "' but does not provide '" +
                    interfaceMethodSignature(im) + "'. Declare the method, or inherit it "
                    "from a base class.");
                continue;
            }
            const MethodInfo& cm = decl->methods[decl->findMethodIndexBySignature(im.name, im.symbol)];
            if (!overrideSignaturesCompatible(im, cm.symbol)) {
                errorAtNode(cd.node, "Method '" + asciiOf(im.name) + "' of '" +
                    asciiOf(si->name) + "' does not match '" + interfaceMethodSignature(im) +
                    "' declared in interface '" + ifaceT->toString() + "'.");
            }
            if (im.isNoreturn && !cm.isNoreturn) {
                errorAtNode(cd.node, "Method '" + asciiOf(im.name) + "' of '" +
                    asciiOf(si->name) + "' must be marked 'noreturn' because interface '" +
                    ifaceT->toString() + "' declares it 'noreturn'; callers rely on it never "
                    "returning.");
            }
        }
    }

    // --- A concrete class must implement every inherited abstract method. ---
    if (!si->isAbstract) {
        std::vector<const Symbol*> checked;
        for (StructInfo* s = si; s; s = s->baseInfo) {
            for (auto& m : s->methods) {
                if (m.isConstructor || m.isDestructor || !m.symbol) continue;
                bool seen = false;
                for (const Symbol* c : checked) {
                    if (c->name == m.name && sameParameterTypes(c, m.symbol)) { seen = true; break; }
                }
                if (seen) continue;
                checked.push_back(m.symbol);
                StructInfo* decl = si->classDeclaringMethodBySignature(m.name, m.symbol);
                if (decl && decl->methods[decl->findMethodIndexBySignature(m.name, m.symbol)].isAbstract) {
                    errorAtNode(cd.node, "Class '" + asciiOf(si->name) +
                        "' must override abstract method '" + asciiOf(m.name) +
                        "', or be declared 'abstract'");
                }
            }
        }
    }
    si->membersCollected = true;
    popTypeParams(tpCount);
}

void Analyzer::layoutDeclaredClasses(const ast::SourceFile& file) {
    auto classes = file.classes();
    std::vector<ast::ClassDecl> order(classes.begin(), classes.end());
    std::stable_sort(order.begin(), order.end(),
        [&](const ast::ClassDecl& a, const ast::ClassDecl& b) {
            Type* ta = analysis.typeOf(a.node.greenNode());
            Type* tb = analysis.typeOf(b.node.greenNode());
            int da = (ta && ta->structInfo) ? baseDepth(ta->structInfo) : 0;
            int db = (tb && tb->structInfo) ? baseDepth(tb->structInfo) : 0;
            return da < db;
        });
    std::vector<StructInfo*> sis;
    for (auto& cd : order) {
        layoutOneClass(cd);
        if (Type* t = analysis.typeOf(cd.node.greenNode()); t && t->structInfo)
            sis.push_back(t->structInfo);
    }
    finalizeClassHierarchy(sis);
    typeCtx.refreshInstantiationInheritance();
}

void Analyzer::finalizeClassHierarchy(const std::vector<StructInfo*>& classes) {
    std::vector<StructInfo*> order(classes.begin(), classes.end());
    std::stable_sort(order.begin(), order.end(),
        [](StructInfo* a, StructInfo* b) { return baseDepth(a) < baseDepth(b); });

    // --- Assign vtable slots: a method is virtual only when abstract or overridden somewhere. ---
    // A base that is a generic instantiation defers slot state to its template
    // (the template is finalized in its own module; the instance copies slots
    // when it fills).
    auto slotAuthority = [](StructInfo* s) -> StructInfo* {
        return (s && s->templateOf) ? s->templateOf : s;
    };
    for (StructInfo* si : order) {  // phase 1: mark
        for (auto& mi : si->methods) {
            if (mi.isConstructor || mi.isDestructor) continue;
            if (mi.isAbstract) mi.vtableSlot = VTSLOT_PENDING;
            if (mi.isOverride && si->baseInfo) {
                if (StructInfo* bc = si->baseInfo->classDeclaringMethodBySignature(mi.name, mi.symbol)) {
                    mi.vtableSlot = VTSLOT_PENDING;
                    int bi = bc->findMethodIndexBySignature(mi.name, mi.symbol);
                    StructInfo* auth = slotAuthority(bc);
                    if (bi >= 0 && bi < static_cast<int>(auth->methods.size())) {
                        auto& baseMethod = auth->methods[bi];
                        if (baseMethod.vtableSlot == -1) baseMethod.vtableSlot = VTSLOT_PENDING;
                    }
                }
            }
        }
    }
    for (StructInfo* si : order) {  // phase 2: assign indices
        StructInfo* baseAuth = slotAuthority(si->baseInfo);
        si->vtableSize = baseAuth ? baseAuth->vtableSize : 0;
        for (auto& mi : si->methods) {
            if (mi.isConstructor || mi.isDestructor || mi.vtableSlot != VTSLOT_PENDING) continue;
            StructInfo* bc = si->baseInfo
                ? si->baseInfo->classDeclaringMethodBySignature(mi.name, mi.symbol) : nullptr;
            int bi = bc ? bc->findMethodIndexBySignature(mi.name, mi.symbol) : -1;
            StructInfo* bcAuth = slotAuthority(bc);
            int inherited = (bcAuth && bi >= 0 && bi < static_cast<int>(bcAuth->methods.size()))
                ? bcAuth->methods[bi].vtableSlot : -1;
            mi.vtableSlot = (inherited >= 0) ? inherited : si->vtableSize++;
        }
    }

    // ABI throws-ness is uniform across a vtable slot
    for (StructInfo* si : order) {
        for (auto& mi : si->methods) {
            if (mi.isConstructor || mi.isDestructor || !mi.symbol) continue;
            StructInfo* root = nullptr;
            int ri = -1;
            for (StructInfo* s = si; s; s = s->baseInfo) {
                int i = s->findMethodIndexBySignature(mi.name, mi.symbol);
                if (i >= 0) { root = s; ri = i; }
            }
            Symbol* rootSym = (root && ri >= 0) ? root->methods[ri].symbol : nullptr;
            mi.symbol->abiThrows = rootSym ? rootSym->declaredThrows : mi.symbol->declaredThrows;
        }
    }
}

void Analyzer::collectFunctions(const ast::SourceFile& file) {
    // Compiler-owned modules ($prelude, $ens_test_runner) are exempt from the
    // entry-point placement rule.
    bool mayDefineMain = modulePath_ == u"main" ||
                         (!modulePath_.empty() && modulePath_[0] == u'$');
    for (auto& fn : file.functions()) {
        auto fname = fn.nameText().value_or(std::u16string{});
        if (fname == u"main" && !mayDefineMain) {
            errorAtNode(fn.node, "Function 'main' is the program entry point and may only be "
                "defined in the main module (src/main.ens); module '" + asciiOf(modulePath_) +
                "' cannot define it.");
        }
        uint32_t fPos = fn.nameToken() ? fn.nameToken()->startOffset() : fn.node.startOffset();
        Symbol* sym = makeSymbol(SymbolKind::Function, fname, nullptr, fPos);
        sym->funcDeclCst = fn.node.greenNode();
        sym->visibility = topLevelVisibility(fn.visibilityModifier(), asciiOf(fname));
        sym->declaredThrows = fn.isThrows();
        sym->abiThrows = fn.isThrows();
        sym->isNoreturn = fn.isNoreturn();

        auto tparams = fn.typeParams();
        size_t tpCount = 0;
        if (!tparams.empty()) {
            sym->isTemplate = true;
            for (auto& tp : tparams) sym->typeParamNames.push_back(tp.nameText().value_or(std::u16string{}));
            sym->typeParamBounds = resolveTypeParamBounds(sym, tparams);
            tpCount = pushTypeParams(sym, sym->typeParamNames, sym->typeParamBounds);
        }
        sym->returnType = fn.returnType() && fn.returnType()->typeReference()
            ? resolveTypeReference(*fn.returnType()->typeReference())
            : typeCtx.getPrimitive(TypeKind::Void);
        checkThrowsClausePlacement(fn, /*isOverridable=*/false, /*isConstructor=*/false);
        checkNoreturnPlacement(fn, /*isConstructor=*/false, /*isDestructor=*/false);
        resolveFunctionParams(fn, sym);
        popTypeParams(tpCount);
        if (fname == u"main" && mayDefineMain) checkEntrySignature(fn, sym);

        if (!globalScope->define(sym)) {
            Symbol* existing = globalScope->lookupLocal(fname);
            if (!existing || existing->kind != SymbolKind::Function) {
                errorAtNode(fn.node, "Duplicate function name '" + asciiOf(fname) + "'");
            } else if (existing->isExternal) {
                errorAtNode(fn.node, "Function '" + asciiOf(fname) +
                    "' cannot share its name with an external function; C symbols cannot be overloaded.");
            } else if (fname == u"main") {
                errorAtNode(fn.node, "Function 'main' cannot be overloaded; it is the program entry point.");
            } else if (existing->isTemplate || sym->isTemplate) {
                errorAtNode(fn.node, "Function '" + asciiOf(fname) +
                    "' cannot be overloaded because one of its declarations is generic; "
                    "generic functions do not support overloading.");
            } else {
                Symbol* last = existing;
                bool duplicate = false;
                for (Symbol* o = existing; o; o = o->nextOverload) {
                    last = o;
                    if (sameParameterTypes(o, sym)) { duplicate = true; break; }
                }
                if (duplicate) {
                    errorAtNode(fn.node, "Function '" + asciiOf(fname) +
                        "' is already declared with the same parameter types; "
                        "overloads must differ in parameter count or types.");
                } else {
                    last->nextOverload = sym;
                    for (Symbol* o = existing; o; o = o->nextOverload) o->isOverloaded = true;
                }
            }
        }
        analysis.setSymbol(fn.node.greenNode(), sym);
    }
}

// Each test declaration becomes a public, zero-parameter, void function whose
// scope name is $test<N> (N = source order). The name repeats across modules,
// so the link-level name is qualified with the module path. Tests carry an
// implicit declared contract of `throws Error`, letting a body throw anything
// (or nothing) while callers handle failures with a single catch (Error).
void Analyzer::collectTests(const ast::SourceFile& file) {
    auto tests = file.tests();
    if (tests.empty()) return;

    const std::string& filename = source.getFilename();
    static const std::string kSuffix = "_test.ens";
    bool inTestFile = filename.size() >= kSuffix.size() &&
        filename.compare(filename.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;

    int index = 0;
    for (auto& td : tests) {
        if (!inTestFile) {
            errorAtNode(td.node, "Test declarations are only allowed in files ending '_test.ens'.");
        }
        if (auto description = td.descriptionText()) {
            if (description->empty()) {
                errorAtNode(td.node, "A test description cannot be empty.");
            } else if (description->find(u'\n') != std::u16string::npos ||
                       description->find(u'\r') != std::u16string::npos) {
                errorAtNode(td.node, "A test description cannot contain a line break.");
            }
        }

        std::u16string name = u"$test";
        for (char c : std::to_string(index)) name.push_back(static_cast<char16_t>(c));
        Symbol* sym = makeSymbol(SymbolKind::Function, name, nullptr, td.node.startOffset());
        sym->funcDeclCst = td.node.greenNode();
        sym->visibility = Visibility::Public;
        sym->declaredThrows = true;
        sym->abiThrows = true;
        sym->linkName = modulePath_.empty() ? name : modulePath_ + u"." + name;
        sym->returnType = typeCtx.getPrimitive(TypeKind::Void);
        globalScope->define(sym);
        analysis.setSymbol(td.node.greenNode(), sym);
        index++;
    }
}

void Analyzer::resolveMethodParams(const ast::FuncDecl& fn, ::Type* receiverType, Symbol* sym,
                                   bool isInterfaceMethod) {
    bool isCtor = fn.isConstructor();

    if (fn.isShorthand() && !isCtor && !fn.isAbstract() && !isInterfaceMethod) {
        errorAtNode(fn.node, "Shorthand declaration ';' is only allowed on a constructor");
    }

    bool seenDefault = false;
    for (auto& p : fn.parameters()) {
        Type* pt = nullptr;
        if (isInterfaceMethod && p.defaultValue()) {
            errorAtNode(p.node, "Interface method parameters cannot have default values; "
                "implementing classes choose their own defaults.");
        }
        if (p.isThisField()) {
            if (!isCtor) {
                errorAtNode(p.node, "'this." + asciiOf(p.nameText().value_or(std::u16string{})) +
                    "' parameters are only allowed in a constructor");
                pt = typeCtx.getError();
            } else if (auto pname = p.nameText()) {
                int idx = receiverType->structInfo->findFieldIndex(*pname);
                if (idx < 0) {
                    errorAtNode(p.node, "No field '" + asciiOf(*pname) + "' on type '" + receiverType->toString() + "'");
                    pt = typeCtx.getError();
                } else {
                    pt = receiverType->structInfo->fields[idx].type;
                }
            } else {
                pt = typeCtx.getError();
            }
        } else if (auto tr = p.typeReference()) {
            pt = resolveTypeReference(*tr);
        } else {
            pt = typeCtx.getError();
        }
        sym->paramTypes.push_back(pt);

        if (p.defaultValue()) {
            seenDefault = true;
        } else if (seenDefault) {
            errorAtNode(p.node, "Parameter '" + asciiOf(p.nameText().value_or(std::u16string{})) +
                "' has no default but follows a defaulted parameter");
        }
    }
}

void Analyzer::resolveFunctionParams(const ast::FuncDecl& fn, Symbol* sym) {
    if (fn.isShorthand()) {
        errorAtNode(fn.node, "Shorthand declaration ';' is only allowed on a constructor");
    }
    bool seenDefault = false;
    for (auto& p : fn.parameters()) {
        if (p.isThisField()) {
            errorAtNode(p.node, "'this." + asciiOf(p.nameText().value_or(std::u16string{})) +
                "' parameters are only allowed in a constructor");
            sym->paramTypes.push_back(typeCtx.getError());
        } else if (auto tr = p.typeReference()) {
            sym->paramTypes.push_back(resolveTypeReference(*tr));
        } else {
            sym->paramTypes.push_back(typeCtx.getError());
        }
        if (p.defaultValue()) {
            seenDefault = true;
        } else if (seenDefault) {
            errorAtNode(p.node, "Parameter '" + asciiOf(p.nameText().value_or(std::u16string{})) +
                "' has no default but follows a defaulted parameter");
        }
    }
}

void Analyzer::checkFieldMethodCollision(StructInfo* owner, const std::u16string& methodName,
                                         bool isConstructor, const SyntaxNode& diag) {
    if (isConstructor || !owner) return;
    int fidx = owner->findFieldIndex(methodName);
    if (fidx < 0) return;
    StructInfo* fieldOwner = owner->fields[fidx].definingClass ? owner->fields[fidx].definingClass : owner;
    errorAtNode(diag, "'" + asciiOf(methodName) + "' is already declared as a field of '" +
        asciiOf(fieldOwner->name) + "'; a field and a method cannot share a name.");
}

// `hash` is a reserved method name with compiler support: any declaration must
// match the synthesized contract so the type stays usable as a hashed key.
void Analyzer::checkHashMethodSignature(const ast::FuncDecl& fn, Symbol* sym, bool isConstructor) {
    if (isConstructor || !sym || sym->name != u"hash") return;
    if (!hashSignatureConforms(sym)) {
        errorAtNode(fn.node, "A method named 'hash' must have the signature 'hash() -> long'; "
            "it defines how values of this type hash when used as keys (for example in a Map or Set).");
    }
}

// `equals` opts a class into content-based '==' and '!=' only when it takes a
// single parameter of the class's own type; that shape is an unambiguous
// equality method. Other methods named `equals` (a no-argument token accessor,
// for instance) are ordinary and untouched. A same-class `equals` must return
// `bool` and cannot fail.
void Analyzer::checkEqualsMethodSignature(const ast::FuncDecl& fn, Symbol* sym, bool isConstructor) {
    if (isConstructor || !sym || sym->name != u"equals" || sym->paramTypes.size() != 1) return;
    Type* p = sym->paramTypes[0];
    if (!p || !p->isClass() || p->structInfo != sym->methodOwner) return;
    if (fn.isThrows()) {
        errorAtNode(fn.throwsToken().value_or(fn.node),
            "A method named 'equals' cannot be marked 'throws'; equality comparison must not fail.");
    }
    if (!sym->returnType || sym->returnType->kind != TypeKind::Bool) {
        std::u16string owner = sym->methodOwner ? sym->methodOwner->name : std::u16string{};
        errorAtNode(fn.node, "A method named 'equals' that takes a '" + asciiOf(owner) +
            "' must return 'bool'; it defines when two instances compare as equal "
            "(for example as Map or Set keys).");
    }
}

// `hash` and `equals` are a matched pair: equal values must hash equally, or
// keyed collections silently misbehave. A class that customizes one must
// customize the other, here or in a base class. Reported once, on the class
// that first supplies a conforming half without its counterpart.
void Analyzer::checkHashEqualsPairing(const ast::ClassDecl& cd, StructInfo* si) {
    if (!si) return;
    auto conformingHash = [](StructInfo* s) {
        for (auto& m : s->methods)
            if (m.name == u"hash" && hashSignatureConforms(m.symbol)) return true;
        return false;
    };
    auto conformingEquals = [](StructInfo* s) {
        for (auto& m : s->methods)
            if (m.name == u"equals" && equalsSignatureConforms(m.symbol)) return true;
        return false;
    };
    // A single same-class parameter marks an equality method even when its
    // return type is wrong; that near-miss gets its own signature diagnostic,
    // so the pairing check treats it as an equals already present.
    auto equalsIntent = [](StructInfo* s) {
        for (auto& m : s->methods) {
            Symbol* sym = m.symbol;
            if (m.name == u"equals" && sym && sym->paramTypes.size() == 1 &&
                sym->paramTypes[0] && sym->paramTypes[0]->isClass() &&
                sym->paramTypes[0]->structInfo == sym->methodOwner) return true;
        }
        return false;
    };
    auto hashNamed = [](StructInfo* s) {
        for (auto& m : s->methods)
            if (m.name == u"hash") return true;
        return false;
    };
    bool ownHash = conformingHash(si);
    bool ownEquals = conformingEquals(si);
    bool equalsAnywhere = false, hashAnywhere = false;
    for (StructInfo* s = si; s; s = s->baseInfo) {
        if (equalsIntent(s)) equalsAnywhere = true;
        if (hashNamed(s)) hashAnywhere = true;
    }
    if (ownHash && !equalsAnywhere) {
        errorAtNode(cd.node, "Class '" + asciiOf(si->name) + "' defines 'hash' but not 'equals'. "
            "A class that customizes one must customize both, so equal values hash equally. "
            "Add an 'equals(" + asciiOf(si->name) + " other) -> bool' method.");
    }
    if (ownEquals && !hashAnywhere) {
        errorAtNode(cd.node, "Class '" + asciiOf(si->name) + "' defines 'equals' but not 'hash'. "
            "A class that customizes one must customize both, so equal values hash equally. "
            "Add a 'hash() -> long' method.");
    }
}

// A struct compares memberwise, so every field's type must have its own '=='.
// Reports the offending field's dotted path and its type when one does not. A
// field whose type mentions a type parameter is judged per instantiation during
// code generation, exactly like an interpolation hole, so it is skipped here.
void Analyzer::checkStructEquatable(Type* structT, const SyntaxNode& node) {
    if (!structT || !structT->structInfo) return;
    std::vector<StructInfo*> visiting;
    std::string fieldPath;
    Type* leaf = nullptr;
    if (findNonComparableField(structT, visiting, fieldPath, leaf) && leaf) {
        errorAtNode(node, "Struct '" + asciiOf(structT->structInfo->name) +
            "' cannot be compared with '=='. Field '" + fieldPath + "' has type '" +
            leaf->toString() + "', which has no '=='; external types have no value equality. "
            "Compare the fields you need directly instead.");
    }
}

// Walks a struct's fields for one whose type has no '=='. Descends through
// nullable wrappers and nested structs, recording the dotted path to the leaf.
// External-typed fields are the only concrete leaf without '=='; arrays and
// classes compare by identity, so they are fine.
bool Analyzer::findNonComparableField(Type* structT, std::vector<StructInfo*>& visiting,
                                      std::string& fieldPath, Type*& leaf) {
    if (!structT || !structT->structInfo) return false;
    for (StructInfo* seen : visiting) {
        if (seen == structT->structInfo) return false;
    }
    visiting.push_back(structT->structInfo);
    for (const FieldInfo& f : structT->structInfo->fields) {
        Type* ft = f.type;
        if (!ft || TypeContext::containsTypeParam(ft)) continue;
        Type* core = ft;
        if (core->isOptional() && core->inner) core = core->inner;
        if (core->isExternal()) {
            fieldPath = asciiOf(f.name);
            leaf = core;
            return true;
        }
        if (core->isStruct()) {
            std::string sub;
            if (findNonComparableField(core, visiting, sub, leaf)) {
                fieldPath = asciiOf(f.name) + "." + sub;
                return true;
            }
        }
    }
    visiting.pop_back();
    return false;
}

// A struct serializes to JSON field by field, so every field's type must have a
// JSON form. Reports the offending field's dotted path and type when one does
// not. A field mentioning a type parameter is judged per instantiation during
// code generation, so it is skipped here.
void Analyzer::checkStructJsonable(Type* structT, const SyntaxNode& node) {
    if (!structT || !structT->structInfo) return;
    std::vector<StructInfo*> visiting;
    std::string fieldPath;
    Type* leaf = nullptr;
    if (findNonJsonableField(structT, visiting, fieldPath, leaf) && leaf) {
        errorAtNode(node, "Struct '" + asciiOf(structT->structInfo->name) +
            "' cannot be converted to a string. Field '" + fieldPath + "' has type '" +
            leaf->toString() + "', which has no string form; only value types, strings, enums, "
            "their nullable forms, and nested such structs serialize to JSON. Convert or drop the field.");
    }
}

// Walks a struct's fields for one with no JSON form. Value types, strings, and
// enums (and their nullable forms) serialize; a class, interface, array, or
// external field does not. Descends through nested structs, recording the dotted
// path to the leaf.
bool Analyzer::findNonJsonableField(Type* structT, std::vector<StructInfo*>& visiting,
                                    std::string& fieldPath, Type*& leaf) {
    if (!structT || !structT->structInfo) return false;
    for (StructInfo* seen : visiting) {
        if (seen == structT->structInfo) return false;
    }
    visiting.push_back(structT->structInfo);
    for (const FieldInfo& f : structT->structInfo->fields) {
        Type* ft = f.type;
        if (!ft || TypeContext::containsTypeParam(ft)) continue;
        Type* core = ft;
        if (core->isOptional() && core->inner) core = core->inner;
        bool jsonableLeaf = core->isInteger() || core->kind == TypeKind::Float ||
            core->kind == TypeKind::Double || core->isBool() || core->isString() || core->isEnum();
        if (jsonableLeaf) continue;
        if (core->isStruct()) {
            std::string sub;
            if (findNonJsonableField(core, visiting, sub, leaf)) {
                fieldPath = asciiOf(f.name) + "." + sub;
                return true;
            }
            continue;
        }
        fieldPath = asciiOf(f.name);
        leaf = core;
        return true;
    }
    visiting.pop_back();
    return false;
}

void Analyzer::checkThrowsClausePlacement(const ast::FuncDecl& fn, bool isOverridable,
                                          bool isConstructor) {
    if (!fn.isThrows()) return;
    SyntaxNode at = fn.throwsToken().value_or(fn.node);
    if (isConstructor) {
        errorAtNode(at, "A constructor cannot be marked 'throws'. Constructors must not let "
            "exceptions escape; handle them with a 'catch' clause after the constructor body.");
        return;
    }
    bool hasTypes = !fn.declaredThrowsTypes().empty();
    if (fn.isAbstract()) {
        if (!hasTypes)
            errorAtNode(at, "An abstract method marked 'throws' must list its exception types, "
                "e.g. 'throws IOError'.");
        return;
    }
    if (hasTypes && !isOverridable) {
        errorAtNode(at, "Explicit 'throws' types are only allowed where the method can be "
            "overridden; the thrown types here are inferred. Write 'throws' with no type list.");
    }
}

// 'noreturn' is legal on free functions and methods, including abstract and interface methods,
// where it is part of the contract. It is never legal on a constructor or destructor, and a
// 'noreturn' callable declares no return type because it returns nothing at all.
void Analyzer::checkNoreturnPlacement(const ast::FuncDecl& fn, bool isConstructor,
                                      bool isDestructor) {
    if (!fn.isNoreturn()) return;
    SyntaxNode at = fn.nameToken().value_or(fn.node);
    if (isConstructor) {
        errorAtNode(at, "A constructor cannot be marked 'noreturn'; a constructor always "
            "returns the new instance. Remove 'noreturn'.");
        return;
    }
    if (isDestructor) {
        errorAtNode(at, "A destructor cannot be marked 'noreturn'; it runs during cleanup and "
            "must return. Remove 'noreturn'.");
        return;
    }
    if (auto rt = fn.returnType()) {
        errorAtNode(rt->node, "A 'noreturn' function declares no return type; it never returns "
            "a value, so remove the '-> ...' clause after the parameter list.");
    }
}

// The entry point is started by the operating system, which passes no arguments and
// takes the returned `int` as the process exit code, so `main` is a plain function
// with no type parameters, no parameters, and either `int` or no return type.
void Analyzer::checkEntrySignature(const ast::FuncDecl& fn, Symbol* sym) {
    auto tparams = fn.typeParams();
    if (!tparams.empty()) {
        errorAtNode(tparams.front().node, "Function 'main' is the program entry point and cannot "
            "be generic, so it cannot declare the type parameter '" +
            asciiOf(tparams.front().nameText().value_or(std::u16string{})) + "'; nothing calls it "
            "with type arguments. Declare it as 'main() -> int' (or 'main()') and put the generic "
            "work in a function that 'main' calls.");
    }
    auto params = fn.parameters();
    if (!params.empty()) {
        errorAtNode(params.front().node, "Function 'main' is the program entry point and takes no "
            "parameters, so it cannot declare '" +
            asciiOf(params.front().nameText().value_or(std::u16string{})) + "'. Declare it as "
            "'main() -> int' (or 'main()') and read the command line with 'system.arguments()' "
            "after 'import @std.system;'.");
    }
    Type* returned = sym->returnType;
    if (returned && !returned->isVoid() && !returned->isError() &&
        returned->kind != TypeKind::Int) {
        SyntaxNode at = fn.node;
        if (auto rt = fn.returnType()) {
            auto tr = rt->typeReference();
            at = tr ? tr->node : rt->node;
        }
        errorAtNode(at, "Function 'main' is the program entry point and must return 'int' or "
            "nothing, not '" + returned->toString() + "'; the value it returns becomes the "
            "process exit code. Write 'main() -> int' to choose the exit code, or 'main()' to "
            "leave it 0.");
    }
}

void Analyzer::checkFieldDefaults(const ast::StructDecl& sd) {
    Type* t = analysis.typeOf(sd.node.greenNode());
    if (!t || !t->structInfo) return;

    Symbol* prevFunction = currentFunction;
    Symbol* prevThis = currentThis;
    Scope* prevScope = currentScope;
    currentFunction = nullptr;
    currentThis = nullptr;
    currentScope = globalScope;
    Scope* fieldScope = pushScope();

    auto fields = sd.fields();
    for (size_t i = 0; i < fields.size(); ++i) {
        auto& f = fields[i];
        auto dv = f.defaultValue();
        Type* expected = (i < t->structInfo->fields.size()) ? t->structInfo->fields[i].type : typeCtx.getError();
        if (dv) {
            if (auto dvExpr = dv->expression()) {
                Type* actual = analyzeExprAdapt(*dvExpr, expected);
                if (!expected->isError() && !actual->isError() && !expected->assignableFrom(actual)) {
                    errorAtNode(dvExpr->node, "Default value for field '" +
                        asciiOf(f.nameText().value_or(std::u16string{})) + "': expected '" +
                        expected->toString() + "', got '" + actual->toString() + "'");
                }
            }
        }
        auto fname = f.nameText();
        if (fname && !fname->empty()) {
            uint32_t fOffset = f.nameToken() ? f.nameToken()->startOffset() : f.node.startOffset();
            Symbol* sib = makeSymbol(SymbolKind::SiblingField, *fname, expected, fOffset);
            sib->siblingFieldIndex = static_cast<int>(i);
            fieldScope->define(sib);
        }
    }

    popScope();
    currentFunction = prevFunction;
    currentThis = prevThis;
    currentScope = prevScope;
}

void Analyzer::checkFieldDefaults(const ast::ClassDecl& cd) {
    Type* t = analysis.typeOf(cd.node.greenNode());
    if (!t || !t->structInfo) return;

    Symbol* prevFunction = currentFunction;
    Symbol* prevThis = currentThis;
    Scope* prevScope = currentScope;
    currentFunction = nullptr;
    currentThis = nullptr;
    currentScope = globalScope;
    Scope* fieldScope = pushScope();

    auto fields = cd.fields();
    for (size_t i = 0; i < fields.size(); ++i) {
        auto& f = fields[i];
        auto dv = f.defaultValue();
        Type* expected = (i < t->structInfo->fields.size()) ? t->structInfo->fields[i].type : typeCtx.getError();
        if (dv) {
            if (auto dvExpr = dv->expression()) {
                Type* actual = analyzeExprAdapt(*dvExpr, expected);
                if (!expected->isError() && !actual->isError() && !expected->assignableFrom(actual)) {
                    errorAtNode(dvExpr->node, "Default value for field '" +
                        asciiOf(f.nameText().value_or(std::u16string{})) + "': expected '" +
                        expected->toString() + "', got '" + actual->toString() + "'");
                }
            }
        }
        auto fname = f.nameText();
        if (fname && !fname->empty()) {
            uint32_t fOffset = f.nameToken() ? f.nameToken()->startOffset() : f.node.startOffset();
            Symbol* sib = makeSymbol(SymbolKind::SiblingField, *fname, expected, fOffset);
            sib->siblingFieldIndex = static_cast<int>(i);
            fieldScope->define(sib);
        }
    }

    popScope();
    currentFunction = prevFunction;
    currentThis = prevThis;
    currentScope = prevScope;
}

void Analyzer::checkParameterDefaults(const ast::FuncDecl& fn) {
    Symbol* prevFunction = currentFunction;
    Symbol* prevThis = currentThis;
    Scope* prevScope = currentScope;
    currentFunction = nullptr;
    currentThis = nullptr;
    currentScope = globalScope;

    Symbol* sym = analysis.find(fn.node.greenNode())
        ? analysis.find(fn.node.greenNode())->resolvedSymbol
        : nullptr;

    auto params = fn.parameters();
    for (size_t i = 0; i < params.size(); ++i) {
        auto& p = params[i];
        auto dv = p.defaultValue();
        if (!dv) continue;
        auto dvExpr = dv->expression();
        if (!dvExpr) continue;
        Type* expected = (sym && i < sym->paramTypes.size()) ? sym->paramTypes[i] : typeCtx.getError();
        Type* actual = analyzeExprAdapt(*dvExpr, expected);
        if (!expected->isError() && !actual->isError() && !expected->assignableFrom(actual)) {
            errorAtNode(dvExpr->node, "Default value for parameter '" +
                asciiOf(p.nameText().value_or(std::u16string{})) + "': expected '" +
                expected->toString() + "', got '" + actual->toString() + "'");
        }
    }

    currentFunction = prevFunction;
    currentThis = prevThis;
    currentScope = prevScope;
}

// =========================================================
// Type references
// =========================================================

std::vector<std::vector<StructInfo*>> Analyzer::resolveTypeParamBounds(
        const void* /*owner*/, const std::vector<ast::TypeParam>& params) {
    std::vector<std::vector<StructInfo*>> all;
    all.reserve(params.size());
    for (auto& tp : params) {
        std::vector<StructInfo*> bounds;
        StructInfo* classBound = nullptr;
        std::string pname = asciiOf(tp.nameText().value_or(std::u16string{}));
        for (auto& br : tp.bounds()) {
            Type* bt = resolveTypeReference(br);
            if (!bt || bt->isError()) continue;
            StructInfo* b = nullptr;
            if (bt->isInterface()) {
                b = bt->structInfo;
            } else if (bt->isClass() && bt->structInfo && !bt->structInfo->isTemplate) {
                b = bt->structInfo;
            } else {
                errorAtNode(br.node, "A type-parameter bound must be a non-generic class or "
                    "an interface; '" + bt->toString() + "' is not");
                continue;
            }
            bool dup = false;
            for (StructInfo* prev : bounds) {
                if (prev == b) { dup = true; break; }
            }
            if (dup) {
                errorAtNode(br.node, "Duplicate bound '" + bt->toString() +
                    "' on type parameter '" + pname + "'; list each bound once.");
                continue;
            }
            if (!b->isInterface) {
                if (classBound) {
                    errorAtNode(br.node, "Type parameter '" + pname + "' already has the class "
                        "bound '" + asciiOf(classBound->name) + "'; at most one bound can be a "
                        "class, every other bound must be an interface.");
                    continue;
                }
                classBound = b;
            }
            bounds.push_back(b);
        }
        all.push_back(std::move(bounds));
    }
    return all;
}

size_t Analyzer::pushTypeParams(const void* owner, const std::vector<std::u16string>& names,
                                const std::vector<std::vector<StructInfo*>>& bounds) {
    static const std::vector<StructInfo*> kNoBounds;
    for (size_t i = 0; i < names.size(); ++i) {
        const std::vector<StructInfo*>& b = i < bounds.size() ? bounds[i] : kNoBounds;
        Type* ph = typeCtx.getTypeParam(owner, static_cast<int>(i), names[i], b);
        typeParamScope_.push_back({names[i], ph});
    }
    return names.size();
}

size_t Analyzer::enterTemplateScope(StructInfo* si, const std::vector<ast::TypeParam>& astParams) {
    if (!si || !si->isTemplate) return 0;
    if (si->typeParamBounds.empty() && !astParams.empty()) {
        si->typeParamBounds = resolveTypeParamBounds(si, astParams);
    }
    return pushTypeParams(si, si->typeParamNames, si->typeParamBounds);
}

void Analyzer::popTypeParams(size_t count) {
    while (count-- > 0 && !typeParamScope_.empty()) typeParamScope_.pop_back();
}

// The compiler-known hashing contract from the standard library. Every type
// satisfies it: hash() is synthesized for types that do not declare their own.
static bool isHashableClass(const StructInfo* si) {
    return si && si->name == u"Hashable" && si->modulePath == u"std.hash";
}

// All bounds of a type-parameter placeholder. Falls back to the primary bound
// for placeholders created before multi-bound support populated paramBounds.
static std::vector<StructInfo*> boundsOfTypeParam(const Type* t) {
    if (!t) return {};
    if (!t->paramBounds.empty()) return t->paramBounds;
    if (t->structInfo) return {t->structInfo};
    return {};
}

bool Analyzer::checkTypeArgBound(Type* arg, const std::vector<StructInfo*>& bounds,
                                 const std::u16string& paramName, const SyntaxNode& diag) {
    bool ok = true;
    for (StructInfo* bound : bounds) {
        if (!bound) continue;
        // Every type satisfies the compiler-known hashing contract.
        if (isHashableClass(bound)) continue;
        bool satisfied = false;
        if (arg && arg->isClass() && arg->structInfo) {
            satisfied = arg->structInfo->isSubclassOrConforms(bound);
        } else if (arg && arg->isTypeParam()) {
            // A type-parameter argument satisfies a bound its own bounds imply.
            for (StructInfo* own : arg->paramBounds) {
                if (own && own->isSubclassOrConforms(bound)) { satisfied = true; break; }
            }
        }
        if (satisfied) continue;
        if (bound->isInterface) {
            errorAtNode(diag, "Type argument '" + (arg ? arg->toString() : std::string("?")) +
                "' for type parameter '" + asciiOf(paramName) + "' must implement interface '" +
                asciiOf(bound->name) + "'");
        } else {
            errorAtNode(diag, "Type argument '" + (arg ? arg->toString() : std::string("?")) +
                "' for type parameter '" + asciiOf(paramName) + "' must be '" + asciiOf(bound->name) +
                "' or a subclass of it");
        }
        ok = false;
    }
    return ok;
}

Type* Analyzer::instantiateFromArgs(Type* templateType,
                                    const std::vector<ast::TypeReference>& args,
                                    const SyntaxNode& diag) {
    StructInfo* tmpl = templateType->structInfo;
    size_t arity = tmpl->typeParamNames.size();
    if (args.size() != arity) {
        errorAtNode(diag, "Generic type '" + asciiOf(tmpl->name) + "' expects " +
            std::to_string(arity) + (arity == 1 ? " type argument" : " type arguments") +
            ", but " + std::to_string(args.size()) + (args.size() == 1 ? " was given" : " were given"));
        return typeCtx.getError();
    }
    std::vector<Type*> argTypes;
    argTypes.reserve(args.size());
    bool ok = true;
    static const std::vector<StructInfo*> kNoBounds;
    for (size_t i = 0; i < args.size(); ++i) {
        Type* at = resolveTypeReference(args[i]);
        if (!at || at->isError()) { ok = false; argTypes.push_back(typeCtx.getError()); continue; }
        argTypes.push_back(at);
        const std::vector<StructInfo*>& bounds =
            i < tmpl->typeParamBounds.size() ? tmpl->typeParamBounds[i] : kNoBounds;
        std::u16string pname = i < tmpl->typeParamNames.size() ? tmpl->typeParamNames[i] : std::u16string{};
        if (!checkTypeArgBound(at, bounds, pname, args[i].node)) ok = false;
    }
    if (!ok) return typeCtx.getError();
    return typeCtx.instantiate(templateType, argTypes);
}

Type* Analyzer::lookupTypeByName(const std::u16string& qualifier,
                                 const std::u16string& name,
                                 const SyntaxNode& diagNode) {
    if (qualifier.empty()) {
        if (Type* prim = typeCtx.primitiveFromName(name)) return prim;
        for (auto it = typeParamScope_.rbegin(); it != typeParamScope_.rend(); ++it) {
            if (it->first == name) return it->second;
        }
        if (Type* t = typeCtx.lookupNamedType(modulePath_, name)) return t;
        // Fall back to imported aliases stored in the module's globalScope.
        if (Symbol* sym = globalScope ? globalScope->lookupLocal(name) : nullptr) {
            if (sym->type && (sym->type->isStruct() || sym->type->isClass() ||
                              sym->type->isEnum() || sym->type->isExternal())) return sym->type;
        }
        errorAtNode(diagNode, "Unknown type '" + asciiOf(name) +
            "'. Check the spelling, or import the module that declares it.");
        return typeCtx.getError();
    }

    Symbol* nsSym = globalScope ? globalScope->lookupLocal(qualifier) : nullptr;
    if (!nsSym || nsSym->kind != SymbolKind::Namespace) {
        errorAtNode(diagNode, "'" + asciiOf(qualifier) + "' is not a namespace alias");
        return typeCtx.getError();
    }
    Type* t = typeCtx.lookupNamedType(nsSym->namespaceModulePath, name);
    if (!t) {
        errorAtNode(diagNode, "Module '" + asciiOf(nsSym->namespaceModulePath) +
            "' has no type '" + asciiOf(name) + "'");
        return typeCtx.getError();
    }
    if (!isTypeVisibleFrom(t)) {
        errorAtNode(diagNode, invisibleTypeMessage(name, t));
        return typeCtx.getError();
    }
    return t;
}

Type* Analyzer::resolveTypeReference(const ast::TypeReference& tr) {
    auto name = tr.nameText();
    if (!name) return typeCtx.getError();
    auto qualifier = tr.qualifierText().value_or(std::u16string{});

    Type* base = lookupTypeByName(qualifier, *name, tr.node);
    if (base->isError()) {
        analysis.setType(tr.node.greenNode(), typeCtx.getError());
        return typeCtx.getError();
    }
    auto typeArgs = tr.typeArguments();
    bool baseIsTemplate = base->structInfo && base->structInfo->isTemplate &&
                          (base->isClass() || base->isStruct());
    if (baseIsTemplate) {
        if (typeArgs.empty()) {
            errorAtNode(tr.node, "Generic type '" + asciiOf(base->structInfo->name) +
                "' requires type arguments (for example '" + asciiOf(base->structInfo->name) + "<int>')");
            analysis.setType(tr.node.greenNode(), typeCtx.getError());
            return typeCtx.getError();
        }
        base = instantiateFromArgs(base, typeArgs, tr.node);
        if (base->isError()) {
            analysis.setType(tr.node.greenNode(), typeCtx.getError());
            return typeCtx.getError();
        }
    } else if (!typeArgs.empty()) {
        errorAtNode(tr.node, "Type '" + base->toString() +
            "' is not generic and takes no type arguments");
        analysis.setType(tr.node.greenNode(), typeCtx.getError());
        return typeCtx.getError();
    }
    Type* result = base;
    auto suffixes = tr.suffixChain();
    for (auto s : suffixes) {
        if (result->isError()) break;
        if (s == ast::TypeReference::Suffix::Array) {
            if (result->isVoid()) {
                errorAtNode(tr.node, "void cannot be an array element type");
                result = typeCtx.getError();
                break;
            }
            // Don't reject types like `int[][]` at name-resolution: multi-dim
            // `new T[a][b]` fully populates inner slots, so the type is fine
            // as long as every value of this type is constructed. The
            // element-nullability rule is enforced where slots actually get
            // zero-initialised (single-dim `new T[size]` and variable/field
            // declarations without an initializer).
            result = typeCtx.getArray(result);
        } else {
            if (result->isVoid()) {
                errorAtNode(tr.node, "void cannot be optional");
                result = typeCtx.getError();
                break;
            }
            result = typeCtx.getOptional(result);
        }
    }
    analysis.setType(tr.node.greenNode(), result);
    return result;
}

// =========================================================
// Function bodies
// =========================================================

void Analyzer::analyzeFunctionBody(const ast::FuncDecl& fn) {
    auto* info = analysis.find(fn.node.greenNode());
    if (!info || !info->resolvedSymbol) return;

    Symbol* prevFunction = currentFunction;
    Symbol* prevThis = currentThis;
    bool prevSawSuper = sawSuperConstructorCall;
    Scope* prevParamScope = currentFunctionParamScope;
    currentFunction = info->resolvedSymbol;
    sawSuperConstructorCall = false;
    int prevLoopDepth = loopDepth;
    loopDepth = 0;

    Scope* funcScope = pushScope();
    currentFunctionParamScope = funcScope;

    Type* receiverType = analysis.receiverOf(fn.node.greenNode());

    size_t tpCount = 0;
    if (currentFunction->isTemplate) {
        tpCount += pushTypeParams(currentFunction, currentFunction->typeParamNames,
                                  currentFunction->typeParamBounds);
    }
    if (receiverType && receiverType->structInfo && receiverType->structInfo->isTemplate) {
        StructInfo* owner = receiverType->structInfo;
        tpCount += pushTypeParams(owner, owner->typeParamNames, owner->typeParamBounds);
    }

    if (receiverType) {
        Symbol* thisSym = makeSymbol(SymbolKind::Parameter, std::u16string(u"this"),
                                     receiverType, fn.node.startOffset());
        currentScope->define(thisSym);
        currentThis = thisSym;
        analysis.setThisSymbol(fn.node.greenNode(), thisSym);
    } else {
        currentThis = nullptr;
    }

    // Bind parameters as locals.
    auto params = fn.parameters();
    for (size_t i = 0; i < params.size(); ++i) {
        auto& p = params[i];
        Type* pt = (i < currentFunction->paramTypes.size())
            ? currentFunction->paramTypes[i] : typeCtx.getError();
        auto pname = p.nameText().value_or(std::u16string{});
        uint32_t pPos = p.nameToken() ? p.nameToken()->startOffset() : p.node.startOffset();
        Symbol* psym = makeSymbol(SymbolKind::Parameter, pname, pt, pPos);
        if (p.isThisField() && receiverType) psym->thisFieldOwner = receiverType->structInfo;
        if (!currentScope->define(psym)) {
            errorAtNode(p.node, "Duplicate parameter name '" + asciiOf(pname) + "'");
        }
        analysis.setSymbol(p.node.greenNode(), psym);
    }

    // Synthesize this.field = paramName for each this-field param.
    if (receiverType) analyzeImplicitConstructorAssignments(fn);

    bool prevDaEnabled = assignmentActive_;
    assignmentActive_ = true;
    resetAssignmentFlow();

    // A constructor also tracks its own fields: a `this.field` shorthand parameter
    // assigns the field before the body runs, and every own non-defaultable field
    // must be assigned on every path that leaves the body normally.
    if (currentFunction->isConstructor && receiverType && receiverType->structInfo) {
        ctorFieldClass_ = receiverType->structInfo;
        for (auto& p : fn.parameters()) {
            if (!p.isThisField()) continue;
            auto pname = p.nameText();
            if (!pname) continue;
            int idx = ctorFieldClass_->findFieldIndex(*pname);
            if (idx >= 0) assignedThisFields_.insert(&ctorFieldClass_->fields[idx]);
        }
        ctorSeededThisFields_ = assignedThisFields_;
    }

    // Body locals live in a child scope so catch clauses (siblings below) can't see them.
    pushScope();
    if (auto body = fn.body()) {
        analyzeStatements(body->statements());
    }
    popScope();

    // Every own non-defaultable field must be assigned where the body falls through.
    if (ctorFieldClass_) checkConstructorFieldsAssigned(fn.node);

    // A constructor must chain to its base when the base has no zero-argument constructor.
    if (receiverType && receiverType->structInfo && !sawSuperConstructorCall) {
        StructInfo* cls = receiverType->structInfo;
        bool isCtor = currentFunction && currentFunction->isConstructor;
        if (isCtor && cls->baseInfo) {
            bool anyCtor = false;
            bool anyCallableWithoutArgs = false;
            for (auto& m : cls->baseInfo->methods) {
                if (!m.isConstructor || !m.symbol) continue;
                anyCtor = true;
                bool callable = requiredArgCount(m.symbol) == 0 &&
                    isMemberAccessAllowed(m.visibility,
                                          m.definingClass ? m.definingClass : cls->baseInfo);
                if (callable) anyCallableWithoutArgs = true;
            }
            if (anyCtor && !anyCallableWithoutArgs) {
                errorAtNode(fn.node, "Constructor of '" + asciiOf(cls->name) +
                    "' must call 'super(...)' because base class '" +
                    asciiOf(cls->baseInfo->name) +
                    "' has no zero-argument constructor callable from here.");
            }
        }
    }

    for (auto& cc : fn.catchClauses()) analyzeCatchClause(cc, funcScope);
    assignmentActive_ = prevDaEnabled;

    checkFunctionReturnPaths(fn);

    popScope();
    popTypeParams(tpCount);
    currentFunction = prevFunction;
    currentThis = prevThis;
    sawSuperConstructorCall = prevSawSuper;
    currentFunctionParamScope = prevParamScope;
    loopDepth = prevLoopDepth;
}

void Analyzer::analyzeTestBody(const ast::TestDecl& test) {
    auto* info = analysis.find(test.node.greenNode());
    if (!info || !info->resolvedSymbol) return;

    Symbol* prevFunction = currentFunction;
    Symbol* prevThis = currentThis;
    Scope* prevParamScope = currentFunctionParamScope;
    int prevLoopDepth = loopDepth;
    currentFunction = info->resolvedSymbol;
    currentThis = nullptr;
    loopDepth = 0;

    Scope* funcScope = pushScope();
    currentFunctionParamScope = funcScope;
    bool prevDaEnabled = assignmentActive_;
    assignmentActive_ = true;
    resetAssignmentFlow();
    pushScope();
    if (auto body = test.body()) {
        analyzeStatements(body->statements());
    }
    popScope();
    popScope();
    assignmentActive_ = prevDaEnabled;

    currentFunction = prevFunction;
    currentThis = prevThis;
    currentFunctionParamScope = prevParamScope;
    loopDepth = prevLoopDepth;
}

void Analyzer::analyzeCatchClause(const ast::CatchClause& clause, Scope* funcScope) {
    currentScope = funcScope;
    pushScope();

    Type* clauseType = clause.typeReference()
        ? resolveTypeReference(*clause.typeReference()) : typeCtx.getError();
    if (!clauseType->isError()) {
        bool isErrorSubclass = clauseType->isClass() && clauseType->structInfo && errorClassInfo_ &&
            clauseType->structInfo->isSubclassOf(errorClassInfo_);
        if (!isErrorSubclass) {
            SyntaxNode diag = clause.typeReference() ? clause.typeReference()->node : clause.node;
            errorAtNode(diag, "Cannot catch '" + clauseType->toString() +
                "'; only 'Error' or a subclass of it can be caught.");
        }
    }

    // A catch body runs on the exception path, where the try body may have thrown
    // before assigning any local or field, so only the caught variable and the
    // fields assigned before the body (the `this.field` parameters) carry in.
    AssignmentFlow prevFlow = snapshotAssignment();
    auto prevBreaks = breakFlows_;
    breakFlows_.clear();
    assignedLocals_.clear();
    assignedThisFields_ = ctorSeededThisFields_;
    flowTerminated_ = false;

    if (auto nameTok = clause.nameToken()) {
        auto cname = clause.nameText().value_or(std::u16string{});
        Symbol* var = makeSymbol(SymbolKind::Variable, cname, clauseType, nameTok->startOffset());
        currentScope->define(var);
        analysis.setSymbol(clause.node.greenNode(), var);
        trackLocal(var, /*assigned=*/true);
    }

    bool prevInCatch = inCatchClause;
    inCatchClause = true;
    if (auto body = clause.body()) {
        analyzeStatements(body->statements());
    }
    inCatchClause = prevInCatch;

    // A catch that falls through without rethrowing returns the object normally, so
    // its fields must be assigned there too.
    if (ctorFieldClass_) checkConstructorFieldsAssigned(clause.node);

    restoreAssignment(prevFlow);
    breakFlows_ = prevBreaks;
    popScope();
}

void Analyzer::analyzeImplicitConstructorAssignments(const ast::FuncDecl& fn) {
    if (!currentThis) return;
    Type* recvType = currentThis->type;
    if (!recvType || !recvType->structInfo) return;

    for (auto& p : fn.parameters()) {
        if (!p.isThisField()) continue;
        auto pname = p.nameText();
        if (!pname) continue;
        int idx = recvType->structInfo->findFieldIndex(*pname);
        if (idx < 0) continue;  // already reported during resolveMethodParams
        Type* fieldType = recvType->structInfo->fields[idx].type;
        Type* paramType = analysis.find(p.node.greenNode())
            ? analysis.find(p.node.greenNode())->resolvedSymbol->type
            : typeCtx.getError();
        if (!fieldType->assignableFrom(paramType)) {
            errorAtNode(p.node, "Cannot assign '" + paramType->toString() +
                "' to field '" + asciiOf(*pname) + "' of type '" + fieldType->toString() + "'");
        }
    }
}

// =========================================================
// Statements
// =========================================================

// `new T[n]` with a non-nullable reference element type is accepted only when
// the allocation is the entire initializer of a fresh local and the next
// statement in the same block is the canonical fill loop:
//     T[] name = new T[n];
//     for (long i = 0; i < name.length; i = i + 1) { name[i] = expr; }
// The loop writes every slot before any read, so the zeroed slots are never
// observed. The shape is matched purely syntactically below.

static std::optional<std::u16string> identExprName(const ast::Expression& e) {
    auto id = e.asIdent();
    if (!id) return std::nullopt;
    return id->nameText();
}

static bool isIntegerLiteralToken(const ast::Expression& e, std::u16string_view text) {
    auto lit = e.asLiteral();
    if (!lit) return false;
    SyntaxKind k = lit->literalKind();
    if (k != SyntaxKind::IntLiteral && k != SyntaxKind::LongLiteral) return false;
    auto tok = lit->token();
    return tok && tok->tokenText() == text;
}

// Any identifier token spelling `name` counts as a mention. Deliberately
// conservative: the fill expression must provably never read the array.
static bool mentionsIdentifier(const SyntaxNode& n, const std::u16string& name) {
    if (n.isToken()) {
        return n.kind() == SyntaxKind::Identifier && n.tokenText() == name;
    }
    for (auto& child : n.children()) {
        if (mentionsIdentifier(child, name)) return true;
    }
    return false;
}

static bool isPlainIndexType(const ast::TypeReference& tr) {
    auto name = tr.nameText();
    if (!name || (*name != u"long" && *name != u"int")) return false;
    return !tr.qualifierText() && tr.typeArguments().empty() && tr.suffixChain().empty();
}

// A step-by-one update on `indexName`: `i = i + 1`, `i++`, or `++i` all count.
static bool isUnitIncrementOf(const ast::Expression& update, const std::u16string& indexName) {
    if (auto step = update.asAssign()) {
        auto stepOp = step->operatorToken();
        if (!stepOp || stepOp->kind() != SyntaxKind::Eq) return false;
        auto stepTarget = step->target();
        if (!stepTarget || identExprName(*stepTarget) != indexName) return false;
        auto stepValue = step->value();
        if (!stepValue) return false;
        auto increment = stepValue->asBinary();
        if (!increment) return false;
        auto incrementOp = increment->operatorToken();
        if (!incrementOp || incrementOp->kind() != SyntaxKind::Plus) return false;
        auto incrementLeft = increment->left();
        if (!incrementLeft || identExprName(*incrementLeft) != indexName) return false;
        auto incrementRight = increment->right();
        return incrementRight && isIntegerLiteralToken(*incrementRight, u"1");
    }
    std::optional<ast::Expression> operand;
    std::optional<SyntaxNode> op;
    if (auto post = update.asPostfix()) { operand = post->operand(); op = post->operatorToken(); }
    else if (auto pre = update.asPrefix()) { operand = pre->operand(); op = pre->operatorToken(); }
    else return false;
    if (!op || op->kind() != SyntaxKind::PlusPlus) return false;
    return operand && identExprName(*operand) == indexName;
}

// Matches `for (long i = 0; i < name.length; i = i + 1) { name[i] = expr; }`
// (the update also accepts `i++`/`++i`) with a fresh `long`/`int` index of any
// name and `expr` never mentioning `name`.
static bool isArrayFillLoop(const ast::ForStatement& loop, const std::u16string& arrayName) {
    std::u16string indexName;
    auto init = loop.init();
    if (!init) return false;
    if (auto let = init->asLet()) {
        auto n = let->nameText();
        auto iv = let->initializer();
        if (!n || !iv || !isIntegerLiteralToken(*iv, u"0")) return false;
        indexName = *n;
    } else if (auto typed = init->asTypedVarDecl()) {
        auto tr = typed->typeReference();
        auto n = typed->nameText();
        auto iv = typed->initializer();
        if (!tr || !isPlainIndexType(*tr)) return false;
        if (!n || !iv || !isIntegerLiteralToken(*iv, u"0")) return false;
        indexName = *n;
    } else {
        return false;
    }
    if (indexName.empty() || indexName == arrayName) return false;

    auto cond = loop.condition();
    if (!cond) return false;
    auto compare = cond->asBinary();
    if (!compare) return false;
    auto compareOp = compare->operatorToken();
    if (!compareOp || compareOp->kind() != SyntaxKind::Lt) return false;
    auto compareLeft = compare->left();
    if (!compareLeft || identExprName(*compareLeft) != indexName) return false;
    auto compareRight = compare->right();
    if (!compareRight) return false;
    auto lengthAccess = compareRight->asMember();
    if (!lengthAccess) return false;
    auto lengthObject = lengthAccess->object();
    if (!lengthObject || identExprName(*lengthObject) != arrayName) return false;
    auto lengthName = lengthAccess->memberText();
    if (!lengthName || *lengthName != u"length") return false;

    auto update = loop.update();
    if (!update || !isUnitIncrementOf(*update, indexName)) return false;

    auto body = loop.body();
    if (!body) return false;
    auto statements = body->statements();
    if (statements.size() != 1) return false;
    auto exprStmt = statements[0].asExpressionStmt();
    if (!exprStmt) return false;
    auto bodyExpr = exprStmt->expression();
    if (!bodyExpr) return false;
    auto slotAssign = bodyExpr->asAssign();
    if (!slotAssign) return false;
    auto slotOp = slotAssign->operatorToken();
    if (!slotOp || slotOp->kind() != SyntaxKind::Eq) return false;
    auto slotTarget = slotAssign->target();
    if (!slotTarget) return false;
    auto subscript = slotTarget->asSubscript();
    if (!subscript) return false;
    auto subscriptObject = subscript->object();
    if (!subscriptObject || identExprName(*subscriptObject) != arrayName) return false;
    auto subscriptIndex = subscript->index();
    if (!subscriptIndex || identExprName(*subscriptIndex) != indexName) return false;
    auto filledValue = slotAssign->value();
    if (!filledValue) return false;
    return !mentionsIdentifier(filledValue->node, arrayName);
}

// Records single-dimension array-new declaration initializers (for diagnostic
// wording) and proves the ones immediately followed by their fill loop.
void Analyzer::noteArrayFillLoop(const std::vector<ast::Statement>& stmts, size_t index) {
    const ast::Statement& stmt = stmts[index];
    std::optional<ast::Expression> init;
    std::optional<std::u16string> name;
    if (auto let = stmt.asLet()) {
        init = let->initializer();
        name = let->nameText();
    } else if (auto typed = stmt.asTypedVarDecl()) {
        init = typed->initializer();
        name = typed->nameText();
    }
    if (!init || !name || name->empty()) return;
    auto arrayNew = init->asNew();
    if (!arrayNew || !arrayNew->isArrayNew()) return;
    if (arrayNew->arraySizeExpressions().size() != 1 ||
        arrayNew->arrayUnsizedTrailingCount() != 0) return;
    arrayNewDeclNames_[arrayNew->node.greenNode()] = *name;
    if (index + 1 >= stmts.size()) return;
    auto loop = stmts[index + 1].asFor();
    if (loop && isArrayFillLoop(*loop, *name)) {
        fillLoopProvenNews_.insert(arrayNew->node.greenNode());
    }
}

void Analyzer::analyzeStatements(const std::vector<ast::Statement>& stmts) {
    for (size_t i = 0; i < stmts.size(); ++i) {
        noteArrayFillLoop(stmts, i);
        analyzeStatement(stmts[i]);
    }
}

void Analyzer::analyzeStatement(const ast::Statement& stmt) {
    if (auto b = stmt.asBlock())              { analyzeBlock(*b); return; }
    if (auto l = stmt.asLet())                { analyzeLetStmt(*l); return; }
    if (auto v = stmt.asTypedVarDecl())       { analyzeTypedVarDeclStmt(*v); return; }
    if (auto i = stmt.asIf())                 { analyzeIfStmt(*i); return; }
    if (auto w = stmt.asWhile())              { analyzeWhileStmt(*w); return; }
    if (auto f = stmt.asFor())                { analyzeForStmt(*f); return; }
    if (auto fe = stmt.asForEach())           { analyzeForEachStmt(*fe); return; }
    if (auto br = stmt.asBreak())             { analyzeBreakStmt(*br); return; }
    if (auto co = stmt.asContinue())          { analyzeContinueStmt(*co); return; }
    if (auto r = stmt.asReturn())             { analyzeReturnStmt(*r); return; }
    if (auto e = stmt.asExpressionStmt())     { analyzeExpressionStmt(*e); return; }
    if (auto th = stmt.asThrow())             { analyzeThrowStmt(*th); return; }
    if (auto rt = stmt.asRethrow())           { analyzeRethrowStmt(*rt); return; }
    if (auto sw = stmt.asSwitch())            { analyzeSwitchStmt(*sw); return; }
}

void Analyzer::analyzeBlock(const ast::Block& block) {
    pushScope();
    analyzeStatements(block.statements());
    popScope();
}

void Analyzer::analyzeLetStmt(const ast::LetStatement& stmt) {
    Type* initType = nullptr;
    if (auto init = stmt.initializer()) initType = analyzeExpr(*init);

    auto name = stmt.nameText().value_or(std::u16string{});
    Type* finalType;
    if (!initType) {
        errorAtNode(stmt.node, "Variable '" + asciiOf(name) + "' needs an initializer");
        finalType = typeCtx.getError();
    } else if (initType->isNull()) {
        errorAtNode(stmt.node, "Cannot infer type from 'null' alone - declare the variable with an explicit type, e.g. 'T? " +
            asciiOf(name) + " = null;'");
        finalType = typeCtx.getError();
    } else {
        finalType = initType;
    }

    uint32_t namePos = stmt.nameToken() ? stmt.nameToken()->startOffset() : stmt.node.startOffset();
    Symbol* sym = makeSymbol(SymbolKind::Variable, name, finalType, namePos);
    sym->isConst = stmt.isConst();
    bool collides = !currentScope->define(sym);
    if (!collides && currentScope->parent == currentFunctionParamScope &&
        currentFunctionParamScope && currentFunctionParamScope->lookupLocal(name)) {
        collides = true;
    }
    if (collides) {
        errorAtNode(stmt.node, "Variable '" + asciiOf(name) + "' is already defined in this scope");
    }
    analysis.setSymbol(stmt.node.greenNode(), sym);
    trackLocal(sym, /*assigned=*/initType != nullptr);
}

void Analyzer::analyzeTypedVarDeclStmt(const ast::TypedVarDeclStatement& stmt) {
    Type* declared = stmt.typeReference()
        ? resolveTypeReference(*stmt.typeReference())
        : typeCtx.getError();
    if (declared->isVoid()) {
        errorAtNode(stmt.node, "Variable cannot have void type");
        declared = typeCtx.getError();
    }
    Type* initType = nullptr;
    if (auto init = stmt.initializer()) initType = analyzeExprAdapt(*init, declared);
    if (initType && !declared->isError() && !declared->assignableFrom(initType)) {
        errorAtNode(stmt.node, "Cannot assign value of type '" + initType->toString() +
            "' to variable of type '" + declared->toString() + "'");
    }
    auto name = stmt.nameText().value_or(std::u16string{});
    if (stmt.isConst() && !initType && !declared->isError()) {
        errorAtNode(stmt.node, "Constant '" + asciiOf(name) + "' must be initialized");
    }
    uint32_t namePos = stmt.nameToken() ? stmt.nameToken()->startOffset() : stmt.node.startOffset();
    Symbol* sym = makeSymbol(SymbolKind::Variable, name, declared, namePos);
    sym->isConst = stmt.isConst();
    bool collides = !currentScope->define(sym);
    if (!collides && currentScope->parent == currentFunctionParamScope &&
        currentFunctionParamScope && currentFunctionParamScope->lookupLocal(name)) {
        collides = true;
    }
    if (collides) {
        errorAtNode(stmt.node, "Variable '" + asciiOf(name) + "' is already defined in this scope");
    }
    if (initType) establishAssignmentNarrowing(sym, initType);
    analysis.setSymbol(stmt.node.greenNode(), sym);
    trackLocal(sym, /*assigned=*/initType != nullptr);
}

static ast::Expression unwrapParens(const ast::Expression& e) {
    ast::Expression cur = e;
    while (auto p = cur.asParen()) {
        auto inner = p->inner();
        if (!inner) break;
        cur = *inner;
    }
    return cur;
}

std::optional<NarrowingPath> Analyzer::buildNarrowingPath(
    const ast::Expression& expr,
    std::vector<Symbol*>* indexSymbols,
    bool allowAnyIndex,
    bool byName) const {
    ast::Expression core = unwrapParens(expr);

    if (auto id = core.asIdent()) {
        Symbol* sym = nullptr;
        if (byName) {
            auto name = id->nameText();
            sym = (name && currentScope) ? currentScope->lookup(*name) : nullptr;
        } else {
            auto* info = analysis.find(id->node.greenNode());
            sym = info ? info->resolvedSymbol : nullptr;
        }
        if (!sym) return std::nullopt;
        if (sym->kind != SymbolKind::Variable && sym->kind != SymbolKind::Parameter) {
            return std::nullopt;
        }
        return NarrowingPath{sym, {}};
    }
    if (core.asThis()) {
        if (!currentThis) return std::nullopt;
        return NarrowingPath{currentThis, {}};
    }
    if (auto m = core.asMember()) {
        auto obj = m->object();
        auto name = m->memberText();
        if (!obj || !name) return std::nullopt;
        auto base = buildNarrowingPath(*obj, indexSymbols, allowAnyIndex, byName);
        if (!base) return std::nullopt;
        PathSegment seg;
        seg.kind = PathSegment::Kind::Field;
        seg.field = *name;
        base->chain.push_back(std::move(seg));
        return base;
    }
    if (auto su = core.asSubscript()) {
        auto obj = su->object();
        auto idx = su->index();
        if (!obj || !idx) return std::nullopt;
        auto base = buildNarrowingPath(*obj, indexSymbols, allowAnyIndex, byName);
        if (!base) return std::nullopt;
        ast::Expression idxCore = unwrapParens(*idx);
        PathSegment seg;
        // An index that is neither an integer literal nor a plain identifier
        // cannot be re-recognized on later reads; for write invalidation it
        // still names some element, so it may alias every one.
        auto unrecognizedIndex = [&]() -> std::optional<NarrowingPath> {
            if (!allowAnyIndex) return std::nullopt;
            PathSegment any;
            any.kind = PathSegment::Kind::AnyIndex;
            base->chain.push_back(std::move(any));
            return base;
        };
        // Integer literal index, possibly with unary minus.
        bool negative = false;
        ast::Expression idxInner = idxCore;
        if (auto pre = idxCore.asPrefix()) {
            if (auto op = pre->operatorToken()) {
                if (op->kind() == SyntaxKind::Minus || op->kind() == SyntaxKind::Plus) {
                    if (auto inner = pre->operand()) {
                        idxInner = *inner;
                        negative = (op->kind() == SyntaxKind::Minus);
                    }
                }
            }
        }
        if (auto lit = idxInner.asLiteral()) {
            if (lit->literalKind() == SyntaxKind::IntLiteral ||
                lit->literalKind() == SyntaxKind::LongLiteral) {
                auto tok = lit->token();
                if (!tok) return unrecognizedIndex();
                uint64_t magnitude = 0;
                if (!parseIntegerLiteralMagnitude(std::u16string(tok->tokenText()), magnitude)) {
                    return unrecognizedIndex();
                }
                seg.kind = PathSegment::Kind::IntIndex;
                seg.intIndex = negative ? -static_cast<int64_t>(magnitude)
                                        : static_cast<int64_t>(magnitude);
                base->chain.push_back(std::move(seg));
                return base;
            }
        }
        // Plain identifier index. Track the symbol so callers can invalidate
        // the narrowing if the index variable is reassigned.
        if (auto idId = idxCore.asIdent()) {
            Symbol* sym = nullptr;
            if (byName) {
                auto idName = idId->nameText();
                sym = (idName && currentScope) ? currentScope->lookup(*idName) : nullptr;
            } else {
                auto* info = analysis.find(idId->node.greenNode());
                sym = info ? info->resolvedSymbol : nullptr;
            }
            if (!sym) return unrecognizedIndex();
            if (sym->kind != SymbolKind::Variable && sym->kind != SymbolKind::Parameter) {
                return unrecognizedIndex();
            }
            seg.kind = PathSegment::Kind::IdentIndex;
            seg.identIndexSym = sym;
            base->chain.push_back(std::move(seg));
            if (indexSymbols) indexSymbols->push_back(sym);
            return base;
        }
        return unrecognizedIndex();
    }
    return std::nullopt;
}

// A value a call can reach - the receiver, or a class / array argument - loses
// every narrowing that passes through its root, since the callee may mutate the
// state reachable from it. The root binding's own narrowing survives: a callee
// cannot reassign the caller's local (or parameter), and the local's strong
// reference keeps the narrowed object alive, so its non-null / type fact holds
// across the call. This applies even when the touched value is itself a member
// chain (`r.door.open()`): only the member paths under `r` drop, `r`'s own fact
// stays. `out` arguments are the exception - the callee writes the caller's
// variable - and are dropped in full on the external-call path.
void Analyzer::clearNarrowingsTouchedBy(const ast::Expression& e) {
    if (!currentScope) return;
    auto p = buildNarrowingPath(e);
    if (!p) return;
    if (p->root &&
        (p->root->kind == SymbolKind::Variable ||
         p->root->kind == SymbolKind::Parameter)) {
        currentScope->clearNarrowingsForRootMembers(p->root);
    } else {
        currentScope->clearNarrowingsForRoot(p->root);
    }
}

// Only class / array references expose mutable state; struct and primitive
// arguments are passed by value, so the call can't mutate through them. A
// constructor call (`new T(...)`) reaches its arguments the same way, so both
// call and `new` route through here.
void Analyzer::clearNarrowingsForArguments(const std::vector<ast::Expression>& args) {
    if (!currentScope) return;
    for (const auto& arg : args) {
        if (arg.asOutArgument()) continue;
        ast::Expression target = arg;
        if (auto na = arg.asNamedArgument()) {
            auto value = na->value();
            if (!value) continue;
            target = *value;
        }
        Type* t = analysis.typeOf(target.node.greenNode());
        if (!t) continue;
        Type* base = t->isOptional() ? t->inner : t;
        if (!base) continue;
        if (!base->isClass() && !base->isArray()) continue;
        clearNarrowingsTouchedBy(target);
    }
}

void Analyzer::clearNarrowingsForCall(const ast::CallExpression& expr) {
    if (!currentScope) return;
    if (auto callee = expr.callee()) {
        if (auto m = callee->asMember()) {
            if (auto obj = m->object()) clearNarrowingsTouchedBy(*obj);
        } else if (auto sm = callee->asSafeMember()) {
            if (auto obj = sm->object()) clearNarrowingsTouchedBy(*obj);
        }
    }
    clearNarrowingsForArguments(expr.arguments());
}

void Analyzer::preClearWrite(const ast::Expression& target) {
    if (!currentScope) return;
    ast::Expression core = unwrapParens(target);
    if (auto id = core.asIdent()) {
        if (auto name = id->nameText()) {
            if (Symbol* sym = currentScope->lookup(*name)) {
                currentScope->clearNarrowingsForRoot(sym);
                currentScope->clearNarrowingsForIndexSymbol(sym);
            }
        }
        return;
    }
    if (core.asMember() || core.asSubscript()) {
        if (auto p = buildNarrowingPath(core, nullptr, /*allowAnyIndex=*/true, /*byName=*/true)) {
            currentScope->clearNarrowingsThatMayAlias(*p);
        }
    }
}

void Analyzer::preClearTouch(const ast::Expression& value) {
    if (!currentScope) return;
    auto p = buildNarrowingPath(value, nullptr, /*allowAnyIndex=*/false, /*byName=*/true);
    if (!p) return;
    if (p->root &&
        (p->root->kind == SymbolKind::Variable ||
         p->root->kind == SymbolKind::Parameter)) {
        currentScope->clearNarrowingsForRootMembers(p->root);
    } else {
        currentScope->clearNarrowingsForRoot(p->root);
    }
}

void Analyzer::preClearLoopBodyWrites(const SyntaxNode& node) {
    if (!currentScope) return;
    ast::Expression e{node};
    if (auto a = e.asAssign()) {
        if (auto t = a->target()) preClearWrite(*t);
    } else if (auto pre = e.asPrefix()) {
        if (auto op = pre->operatorToken()) {
            SyntaxKind k = op->kind();
            if ((k == SyntaxKind::PlusPlus || k == SyntaxKind::MinusMinus) && pre->operand()) {
                preClearWrite(*pre->operand());
            }
        }
    } else if (auto post = e.asPostfix()) {
        if (auto op = post->operatorToken()) {
            SyntaxKind k = op->kind();
            if ((k == SyntaxKind::PlusPlus || k == SyntaxKind::MinusMinus) && post->operand()) {
                preClearWrite(*post->operand());
            }
        }
    } else if (auto oa = e.asOutArgument()) {
        if (auto name = oa->nameText()) {
            if (Symbol* local = currentScope->lookup(*name)) {
                currentScope->clearNarrowingsForRoot(local);
                currentScope->clearNarrowingsForIndexSymbol(local);
            }
        }
    } else if (auto call = e.asCall()) {
        if (auto callee = call->callee()) {
            if (auto m = callee->asMember()) {
                if (auto obj = m->object()) preClearTouch(*obj);
            } else if (auto sm = callee->asSafeMember()) {
                if (auto obj = sm->object()) preClearTouch(*obj);
            }
        }
        for (const auto& arg : call->arguments()) {
            if (arg.asOutArgument()) continue;
            ast::Expression argTarget = arg;
            if (auto na = arg.asNamedArgument()) {
                auto value = na->value();
                if (!value) continue;
                argTarget = *value;
            }
            preClearTouch(argTarget);
        }
    } else if (auto nw = e.asNew()) {
        for (const auto& arg : nw->arguments()) {
            if (arg.asOutArgument()) continue;
            ast::Expression argTarget = arg;
            if (auto na = arg.asNamedArgument()) {
                auto value = na->value();
                if (!value) continue;
                argTarget = *value;
            }
            preClearTouch(argTarget);
        }
    }
    for (const auto& child : node.children()) preClearLoopBodyWrites(child);
}

Analyzer::NullCheckInfo Analyzer::detectNullCheck(const ast::Expression& cond) {
    NullCheckInfo info;
    ast::Expression condCore = unwrapParens(cond);
    auto bin = condCore.asBinary();
    if (!bin) return info;
    auto opTok = bin->operatorToken();
    if (!opTok) return info;
    SyntaxKind op = opTok->kind();
    if (op != SyntaxKind::EqEq && op != SyntaxKind::NotEq) return info;
    auto left = bin->left();
    auto right = bin->right();
    if (!left || !right) return info;
    ast::Expression leftCore = unwrapParens(*left);
    ast::Expression rightCore = unwrapParens(*right);

    auto isNullLit = [](const ast::Expression& e) {
        auto lit = e.asLiteral();
        return lit && lit->literalKind() == SyntaxKind::KwNull;
    };

    const ast::Expression* refSide = nullptr;
    if (isNullLit(rightCore) && !isNullLit(leftCore)) refSide = &leftCore;
    else if (isNullLit(leftCore) && !isNullLit(rightCore)) refSide = &rightCore;
    else return info;

    Type* refType = analysis.typeOf(refSide->node.greenNode());
    if (!refType || !refType->isOptional() || !refType->inner) return info;

    auto path = buildNarrowingPath(*refSide);
    if (!path) return info;

    info.key = std::move(*path);
    info.narrowedT = refType->inner;
    info.narrowsThen = (op == SyntaxKind::NotEq);
    info.valid = true;
    return info;
}

// A type test proves its operand is the target class exactly when the test
// succeeds, so the fact carries only positive polarity: failing a bare `is`
// proves nothing about the operand's type. A `!` around the test flips the
// polarity in collectNarrowings, which is why `!(x is C)` narrows in the else
// branch and past an always-exiting guard.
Analyzer::NullCheckInfo Analyzer::detectTypeTest(const ast::TypeTestExpression& test) {
    NullCheckInfo info;
    auto operand = test.operand();
    auto tr = test.targetType();
    if (!operand || !tr) return info;
    Type* testT = analysis.typeOf(test.node.greenNode());
    if (!testT || !testT->isBool()) return info;  // the test itself did not analyze cleanly
    Type* targetT = analysis.typeOf(tr->node.greenNode());
    if (!targetT || !targetT->isClass()) return info;
    auto path = buildNarrowingPath(*operand);
    if (!path) return info;
    info.key = std::move(*path);
    info.narrowedT = targetT;
    info.narrowsThen = true;
    info.valid = true;
    return info;
}

void Analyzer::collectNarrowings(const ast::Expression& cond, bool conditionHolds,
                                 std::vector<NullCheckInfo>& out) {
    ast::Expression core = unwrapParens(cond);
    if (auto pre = core.asPrefix()) {
        auto opTok = pre->operatorToken();
        if (opTok && opTok->kind() == SyntaxKind::Bang) {
            if (auto inner = pre->operand()) collectNarrowings(*inner, !conditionHolds, out);
            return;
        }
    }
    if (auto tt = core.asTypeTest()) {
        NullCheckInfo info = detectTypeTest(*tt);
        if (info.valid && conditionHolds) out.push_back(std::move(info));
        return;
    }
    if (auto bin = core.asBinary()) {
        auto opTok = bin->operatorToken();
        SyntaxKind op = opTok ? opTok->kind() : SyntaxKind::Invalid;
        bool descend = (conditionHolds && op == SyntaxKind::AmpAmp) ||
                       (!conditionHolds && op == SyntaxKind::PipePipe);
        if (descend) {
            if (auto l = bin->left()) collectNarrowings(*l, conditionHolds, out);
            if (auto r = bin->right()) collectNarrowings(*r, conditionHolds, out);
            return;
        }
    }
    NullCheckInfo info = detectNullCheck(core);
    if (info.valid && info.narrowsThen == conditionHolds) out.push_back(std::move(info));
}

void Analyzer::analyzeBranchWithNarrowing(const ast::Block& block,
                                          const std::vector<NullCheckInfo>& narrowings) {
    pushScope();
    for (const auto& info : narrowings) {
        currentScope->narrowedTypes[info.key] = info.narrowedT;
    }
    analyzeStatements(block.statements());
    popScope();
}

Analyzer::NarrowingSnapshot Analyzer::captureNarrowings() const {
    NarrowingSnapshot snap;
    for (Scope* s = currentScope; s; s = s->parent) {
        snap.layers.push_back({s, s->narrowedTypes});
    }
    return snap;
}

void Analyzer::restoreNarrowings(const NarrowingSnapshot& snap) {
    for (const auto& [scope, map] : snap.layers) {
        scope->narrowedTypes = map;
    }
}

Analyzer::NarrowingFacts Analyzer::flattenNarrowings() const {
    NarrowingFacts flat;
    for (Scope* s = currentScope; s; s = s->parent) {
        for (const auto& [path, type] : s->narrowedTypes) flat.emplace(path, type);
    }
    return flat;
}

Analyzer::NarrowingFacts Analyzer::analyzeBranchCapturing(
        const ast::Block& block, const std::vector<NullCheckInfo>& narrowings) {
    pushScope();
    for (const auto& info : narrowings) {
        currentScope->narrowedTypes[info.key] = info.narrowedT;
    }
    analyzeStatements(block.statements());
    NarrowingFacts facts = flattenNarrowings();
    popScope();
    return facts;
}

Analyzer::NarrowingFacts Analyzer::factsFromNarrowings(
        const std::vector<NullCheckInfo>& narrowings) {
    pushScope();
    for (const auto& info : narrowings) {
        currentScope->narrowedTypes[info.key] = info.narrowedT;
    }
    NarrowingFacts facts = flattenNarrowings();
    popScope();
    return facts;
}

Type* Analyzer::unifyNarrowedTypes(Type* a, Type* b) {
    if (a == b || a->equals(b)) return a;
    if (a->assignableFrom(b)) return a;
    if (b->assignableFrom(a)) return b;
    return nullptr;
}

bool Analyzer::pathRootInScope(const NarrowingPath& path) const {
    if (path.root == currentThis) return true;
    if (!currentScope || !path.root) return false;
    return currentScope->lookup(path.root->name) == path.root;
}

void Analyzer::applyNarrowingJoin(const NarrowingFacts& entry,
                                  const std::vector<NarrowingFacts>& survivors) {
    if (survivors.empty()) return;
    NarrowingFacts joined = survivors.front();
    for (size_t i = 1; i < survivors.size(); ++i) {
        const NarrowingFacts& other = survivors[i];
        NarrowingFacts merged;
        for (const auto& [path, type] : joined) {
            auto it = other.find(path);
            if (it == other.end()) continue;
            if (Type* u = unifyNarrowedTypes(type, it->second)) merged.emplace(path, u);
        }
        joined.swap(merged);
    }
    auto eraseEverywhere = [&](const NarrowingPath& path) {
        for (Scope* s = currentScope; s; s = s->parent) s->narrowedTypes.erase(path);
    };
    for (const auto& [path, type] : entry) {
        if (joined.find(path) == joined.end()) eraseEverywhere(path);
    }
    for (const auto& [path, type] : joined) {
        auto it = entry.find(path);
        if (it != entry.end() && it->second->equals(type)) continue;
        if (!pathRootInScope(path)) continue;
        eraseEverywhere(path);
        currentScope->narrowedTypes[path] = type;
    }
}

// =========================================================
// Missing-return analysis
// =========================================================
//
// Conservative reachability for functions with a non-void return type: every
// path through the body must end in return, throw, rethrow, or panic(). Loops
// are not proven infinite, except `while (true)` with no break out of it.

static bool statementContainsLoopBreak(const ast::Statement& s);

static bool blockContainsLoopBreak(const ast::Block& block) {
    for (auto& child : block.statements()) {
        if (statementContainsLoopBreak(child)) return true;
    }
    return false;
}

// True when the statement contains a `break` that binds to the enclosing loop.
// Nested loops are not entered: a break inside them binds to that inner loop.
static bool statementContainsLoopBreak(const ast::Statement& s) {
    if (s.asBreak()) return true;
    if (auto b = s.asBlock()) return blockContainsLoopBreak(*b);
    if (auto i = s.asIf()) {
        if (auto then = i->thenBlock()) {
            if (blockContainsLoopBreak(*then)) return true;
        }
        if (auto ec = i->elseClause()) {
            if (auto inner = ec->ifStatement()) {
                return statementContainsLoopBreak(ast::Statement{inner->node});
            }
            if (auto bb = ec->block()) return blockContainsLoopBreak(*bb);
        }
        return false;
    }
    if (auto sw = s.asSwitch()) {
        for (auto& arm : sw->arms()) {
            auto bn = arm.bodyBlockNode();
            if (!bn) continue;
            auto blk = ast::Block::cast(*bn);
            if (blk && blockContainsLoopBreak(*blk)) return true;
        }
        return false;
    }
    return false;
}

// A call ends the path when its target is declared `noreturn` (the builtin `panic` among them).
// `try noreturnCall()` also ends it: the call either throws or never returns.
bool Analyzer::isNoreturnCall(const ast::Expression& expr) const {
    ast::Expression core = unwrapParens(expr);
    if (auto t = core.asTry()) {
        if (auto op = t->operand()) core = unwrapParens(*op);
    }
    auto call = core.asCall();
    if (!call) return false;
    auto callee = call->callee();
    if (!callee) return false;
    const auto* info = analysis.find(callee->node.greenNode());
    if (!info) return false;
    Symbol* sym = info->resolvedMethodSymbol ? info->resolvedMethodSymbol : info->resolvedSymbol;
    return sym && sym->isNoreturn;
}

bool Analyzer::blockTerminates(const ast::Block& block, ExitScope scope) const {
    for (auto& s : block.statements()) {
        if (statementTerminates(s, scope)) return true;
    }
    return false;
}

bool Analyzer::ifStatementTerminates(const ast::IfStatement& stmt, ExitScope scope) const {
    auto then = stmt.thenBlock();
    auto ec = stmt.elseClause();
    if (!then || !ec || !blockTerminates(*then, scope)) return false;
    if (auto inner = ec->ifStatement()) return ifStatementTerminates(*inner, scope);
    if (auto bb = ec->block()) return blockTerminates(*bb, scope);
    return false;
}

// The analyzer separately rejects a non-exhaustive switch statement (an enum
// switch must cover every member or have a default, an int/string switch must
// have a default, and a sealed type switch must cover every subclass), so a
// switch terminates when every arm body is a block that terminates.
bool Analyzer::switchStatementTerminates(const ast::SwitchStatement& stmt, ExitScope scope) const {
    auto arms = stmt.arms();
    if (arms.empty()) return false;
    for (auto& arm : arms) {
        auto bn = arm.bodyBlockNode();
        if (!bn) return false;
        auto blk = ast::Block::cast(*bn);
        if (!blk || !blockTerminates(*blk, scope)) return false;
    }
    return true;
}

bool Analyzer::whileStatementTerminates(const ast::WhileStatement& stmt) const {
    auto cond = stmt.condition();
    if (!cond) return false;
    auto lit = unwrapParens(*cond).asLiteral();
    if (!lit || lit->literalKind() != SyntaxKind::KwTrue) return false;
    auto body = stmt.body();
    return !body || !blockContainsLoopBreak(*body);
}

bool Analyzer::statementTerminates(const ast::Statement& s, ExitScope scope) const {
    if (s.asReturn() || s.asThrow() || s.asRethrow()) return true;
    if (scope == ExitScope::Branch && (s.asBreak() || s.asContinue())) return true;
    if (auto b = s.asBlock()) return blockTerminates(*b, scope);
    if (auto i = s.asIf()) return ifStatementTerminates(*i, scope);
    if (auto sw = s.asSwitch()) return switchStatementTerminates(*sw, scope);
    // A nested loop keeps function semantics: a break/continue inside it binds to
    // that loop, not to the branch, so only a proven-infinite loop exits here.
    if (auto w = s.asWhile()) return whileStatementTerminates(*w);
    if (auto e = s.asExpressionStmt()) {
        auto expr = e->expression();
        return expr && isNoreturnCall(*expr);
    }
    return false;
}

void Analyzer::checkFunctionReturnPaths(const ast::FuncDecl& fn) {
    // A `noreturn` body must be unable to complete normally: every path ends by throwing,
    // calling `panic` or another `noreturn` function, or looping forever. A bodiless form
    // (abstract or interface method) carries the contract without a body to check.
    if (currentFunction && currentFunction->isNoreturn) {
        auto body = fn.body();
        if (!body) return;
        std::string name = asciiOf(currentFunction->name);
        if (!blockTerminates(*body, ExitScope::Function)) {
            errorAtNode(fn.nameToken() ? *fn.nameToken() : fn.node, "Function '" + name +
                "' is declared 'noreturn' but can reach the end of its body. Every path must "
                "end by throwing, calling 'panic', or calling another 'noreturn' function.");
        }
        for (auto& cc : fn.catchClauses()) {
            auto cb = cc.body();
            if (cb && !blockTerminates(*cb, ExitScope::Function)) {
                errorAtNode(cc.node, "Function '" + name + "' is declared 'noreturn' but its "
                    "'catch' clause can complete normally. End it by throwing, calling 'panic', "
                    "or calling another 'noreturn' function.");
            }
        }
        return;
    }
    Type* ret = currentFunction ? currentFunction->returnType : nullptr;
    if (!ret || ret->isVoid() || ret->isError()) return;
    auto body = fn.body();
    if (!body) return;
    std::string name = asciiOf(currentFunction->name);
    if (!blockTerminates(*body, ExitScope::Function)) {
        errorAtNode(fn.nameToken() ? *fn.nameToken() : fn.node, "Function '" + name +
            "' can reach the end of its body without returning a value");
        return;
    }
    for (auto& cc : fn.catchClauses()) {
        auto cb = cc.body();
        if (cb && !blockTerminates(*cb, ExitScope::Function)) {
            errorAtNode(cc.node, "Function '" + name +
                "' can reach the end of its body without returning a value");
        }
    }
}

// =========================================================
// Definite assignment
// =========================================================

static bool fieldHasDefaultValue(const FieldInfo& f);

void Analyzer::resetAssignmentFlow() {
    assignedLocals_.clear();
    trackedLocals_.clear();
    breakFlows_.clear();
    flowTerminated_ = false;
    unconditionalPosition_ = true;
    assignmentTargetGreen_ = nullptr;
    ctorFieldClass_ = nullptr;
    assignedThisFields_.clear();
    ctorSeededThisFields_.clear();
}

void Analyzer::trackLocal(Symbol* sym, bool assigned) {
    if (!assignmentActive_ || !sym) return;
    trackedLocals_.insert(sym);
    if (assigned) assignedLocals_.insert(sym);
    else assignedLocals_.erase(sym);
}

void Analyzer::markAssigned(Symbol* sym) {
    if (!assignmentActive_ || !sym || flowTerminated_) return;
    if (trackedLocals_.count(sym)) assignedLocals_.insert(sym);
}

void Analyzer::markThisFieldAssigned(const FieldInfo* field) {
    if (!assignmentActive_ || !field || flowTerminated_ || !ctorFieldClass_) return;
    assignedThisFields_.insert(field);
}

void Analyzer::checkDefiniteAssignment(const ast::IdentExpression& expr, Symbol* sym) {
    if (!assignmentActive_ || !sym || flowTerminated_) return;
    if (expr.node.greenNode() == assignmentTargetGreen_) return;
    if (!trackedLocals_.count(sym) || assignedLocals_.count(sym)) return;
    errorAtNode(expr.node, "Variable '" + asciiOf(sym->name) +
        "' is used before it is assigned a value. Assign it on every path that "
        "reaches this point first, for example '" + asciiOf(sym->name) + " = ...;'.");
    // Suppress cascading reports for the same local by treating it as assigned.
    assignedLocals_.insert(sym);
}

void Analyzer::checkConstructorFieldsAssigned(const SyntaxNode& diag) {
    if (!ctorFieldClass_ || flowTerminated_) return;
    std::vector<const FieldInfo*> missing;
    int baseCount = ctorFieldClass_->baseFieldCount;
    for (size_t i = 0; i < ctorFieldClass_->fields.size(); ++i) {
        if (static_cast<int>(i) < baseCount) continue;
        const FieldInfo& f = ctorFieldClass_->fields[i];
        if (isDefaultable(f.type)) continue;
        if (fieldHasDefaultValue(f)) continue;
        if (assignedThisFields_.count(&f)) continue;
        missing.push_back(&f);
    }
    if (missing.empty()) return;

    std::string cname = asciiOf(ctorFieldClass_->name);
    std::string names;
    for (size_t i = 0; i < missing.size(); ++i) {
        if (i > 0) names += i + 1 == missing.size() ? " and " : ", ";
        names += "'" + asciiOf(missing[i]->name) + "'";
    }
    std::string subject = missing.size() == 1 ? "Field " : "Fields ";
    std::string verb = missing.size() == 1 ? "is" : "are";
    std::string example = "'this." + asciiOf(missing[0]->name) + " = ...;'";
    errorAtNode(diag, subject + names + " of class '" + cname + "' " + verb +
        " not assigned on every path through this constructor. A non-nullable field must "
        "be assigned before the constructor returns; assign it on every path, for example " +
        example + ".");
    // Credit the reported fields so a later exit does not report them again.
    for (const FieldInfo* f : missing) assignedThisFields_.insert(f);
}

Analyzer::AssignmentFlow Analyzer::snapshotAssignment() const {
    return AssignmentFlow{assignedLocals_, assignedThisFields_, flowTerminated_};
}

void Analyzer::restoreAssignment(const AssignmentFlow& flow) {
    assignedLocals_ = flow.assigned;
    assignedThisFields_ = flow.assignedFields;
    flowTerminated_ = flow.terminated;
}

// The join of several branch out-states: a local is assigned after the join only
// if it is assigned on every branch that can fall through. A terminated branch is
// the identity for the intersection (it cannot reach the join). When every branch
// terminates the join is unreachable, so it stays terminated and, to avoid
// spurious reports in the dead code that follows, credits every branch's writes.
Analyzer::AssignmentFlow Analyzer::joinAssignment(const std::vector<AssignmentFlow>& flows) const {
    AssignmentFlow out;
    out.terminated = true;
    bool first = true;
    for (const auto& f : flows) {
        if (f.terminated) continue;
        if (first) {
            out.assigned = f.assigned;
            out.assignedFields = f.assignedFields;
            out.terminated = false;
            first = false;
        } else {
            std::unordered_set<Symbol*> both;
            for (Symbol* s : out.assigned) {
                if (f.assigned.count(s)) both.insert(s);
            }
            out.assigned.swap(both);
            std::unordered_set<const FieldInfo*> bothFields;
            for (const FieldInfo* fld : out.assignedFields) {
                if (f.assignedFields.count(fld)) bothFields.insert(fld);
            }
            out.assignedFields.swap(bothFields);
        }
    }
    if (out.terminated) {
        for (const auto& f : flows) {
            for (Symbol* s : f.assigned) out.assigned.insert(s);
            for (const FieldInfo* fld : f.assignedFields) out.assignedFields.insert(fld);
        }
    }
    return out;
}

void Analyzer::analyzeIfStmt(const ast::IfStatement& stmt) {
    std::vector<NullCheckInfo> whenTrue;
    std::vector<NullCheckInfo> whenFalse;
    if (auto c = stmt.condition()) {
        Type* ct = analyzeExpr(*c);
        if (!ct->isError() && !ct->isBool()) {
            errorAtNode(c->node, "If condition must be 'bool', got '" + ct->toString() + "'");
        }
        collectNarrowings(*c, /*conditionHolds=*/true, whenTrue);
        collectNarrowings(*c, /*conditionHolds=*/false, whenFalse);
    }
    auto thenBlock = stmt.thenBlock();
    auto elseClause = stmt.elseClause();
    AssignmentFlow entryAssign = snapshotAssignment();
    NarrowingSnapshot entryNarrowing = captureNarrowings();
    NarrowingFacts entryFacts = flattenNarrowings();

    NarrowingFacts thenFacts = thenBlock ? analyzeBranchCapturing(*thenBlock, whenTrue)
                                         : factsFromNarrowings(whenTrue);
    AssignmentFlow afterThen = snapshotAssignment();
    restoreAssignment(entryAssign);
    restoreNarrowings(entryNarrowing);

    NarrowingFacts elseFacts;
    AssignmentFlow afterElse = entryAssign;
    if (elseClause) {
        if (auto inner = elseClause->ifStatement()) {
            analyzeIfStmt(*inner);
            elseFacts = flattenNarrowings();
            afterElse = snapshotAssignment();
        } else if (auto bb = elseClause->block()) {
            elseFacts = analyzeBranchCapturing(*bb, whenFalse);
            afterElse = snapshotAssignment();
        } else {
            elseFacts = factsFromNarrowings(whenFalse);
        }
    } else {
        elseFacts = factsFromNarrowings(whenFalse);
    }
    restoreAssignment(entryAssign);
    restoreNarrowings(entryNarrowing);

    restoreAssignment(joinAssignment({afterThen, afterElse}));

    // The narrowing after the `if` is the intersection of the facts holding at the
    // end of each branch that can fall through; a terminated branch contributes
    // nothing, so the surviving branch's facts pass through unchanged. This
    // subsumes the old single-terminating-branch carry: `if (x == null) { return; }`
    // leaves the else facts, and `if (x == null) { x = f(); }` narrows x below
    // because both the reassigned then-branch and the failed-check else prove it.
    std::vector<NarrowingFacts> survivors;
    if (!afterThen.terminated) survivors.push_back(std::move(thenFacts));
    if (!afterElse.terminated) survivors.push_back(std::move(elseFacts));
    applyNarrowingJoin(entryFacts, survivors);
}

// A loop's body may run zero times, so a write inside it is not definitely
// assigned after the loop; only the pre-loop state and the state at each `break`
// reach the normal exit. A `while (true)` / `for (;;)` with no break never exits
// normally, so the code after it is unreachable.
static bool isBoolTrueLiteral(const ast::Expression& e) {
    if (auto lit = e.asLiteral()) return lit->literalKind() == SyntaxKind::KwTrue;
    return false;
}

void Analyzer::analyzeWhileStmt(const ast::WhileStatement& stmt) {
    // The condition is rechecked every iteration and writes inside the body
    // drop narrowing at that point, so condition narrowing is safe in the body.
    // A narrowing established before the loop, however, is stale once a later
    // body write kills it, so drop every such narrowing up front.
    if (auto b = stmt.body()) preClearLoopBodyWrites(b->node);
    std::vector<NullCheckInfo> narrowings;
    bool alwaysTrue = false;
    if (auto c = stmt.condition()) {
        Type* ct = analyzeExpr(*c);
        if (!ct->isError() && !ct->isBool()) {
            errorAtNode(c->node, "While condition must be 'bool', got '" + ct->toString() + "'");
        }
        collectNarrowings(*c, /*conditionHolds=*/true, narrowings);
        alwaysTrue = isBoolTrueLiteral(*c);
    }
    AssignmentFlow postCond = snapshotAssignment();
    breakFlows_.push_back({});
    loopDepth++;
    if (auto b = stmt.body()) analyzeBranchWithNarrowing(*b, narrowings);
    loopDepth--;
    std::vector<AssignmentFlow> exits = std::move(breakFlows_.back());
    breakFlows_.pop_back();
    if (!alwaysTrue) exits.push_back(postCond);
    if (exits.empty()) {
        AssignmentFlow dead = postCond;
        dead.terminated = true;
        restoreAssignment(dead);
    } else {
        restoreAssignment(joinAssignment(exits));
    }
}

void Analyzer::analyzeForStmt(const ast::ForStatement& stmt) {
    pushScope();  // the init binding is scoped to the loop
    if (auto init = stmt.init()) analyzeStatement(*init);
    // A narrowing established before the loop is stale once a later body or
    // update write kills it, so drop every such narrowing before the condition,
    // which then re-establishes its own narrowing for each iteration.
    if (auto b = stmt.body()) preClearLoopBodyWrites(b->node);
    if (auto u = stmt.update()) preClearLoopBodyWrites(u->node);
    std::vector<NullCheckInfo> narrowings;
    bool hasCondition = false;
    bool alwaysTrue = false;
    if (auto c = stmt.condition()) {
        hasCondition = true;
        Type* ct = analyzeExpr(*c);
        if (!ct->isError() && !ct->isBool()) {
            errorAtNode(c->node, "For condition must be 'bool', got '" + ct->toString() + "'");
        }
        collectNarrowings(*c, /*conditionHolds=*/true, narrowings);
        alwaysTrue = isBoolTrueLiteral(*c);
    }
    AssignmentFlow postCond = snapshotAssignment();
    // The update runs after each iteration, so it is analyzed after the body
    // under the same narrowing; a body write drops the narrowing first.
    breakFlows_.push_back({});
    loopDepth++;
    pushScope();
    for (const auto& info : narrowings) {
        currentScope->narrowedTypes[info.key] = info.narrowedT;
    }
    if (auto b = stmt.body()) {
        analyzeStatements(b->statements());
    }
    if (auto u = stmt.update()) { unconditionalPosition_ = true; analyzeExpr(*u); }
    popScope();
    loopDepth--;
    std::vector<AssignmentFlow> exits = std::move(breakFlows_.back());
    breakFlows_.pop_back();
    if (hasCondition && !alwaysTrue) exits.push_back(postCond);
    if (exits.empty()) {
        AssignmentFlow dead = postCond;
        dead.terminated = true;
        restoreAssignment(dead);
    } else {
        restoreAssignment(joinAssignment(exits));
    }
    popScope();  // the init-binding scope opened at the top
}

// True when `si` is the std.collections.iterator Iterable interface (or an instantiation of it).
static bool isIterableInterface(const StructInfo* si) {
    if (!si || !si->isInterface) return false;
    const StructInfo* authority = si->templateOf ? si->templateOf : si;
    return authority->name == u"Iterable" && authority->modulePath == u"std.collections.iterator";
}

// The element type a class yields in a for-in loop. Iteration is nominal: the
// class (or a base class) must implement 'Iterable<T>' from @std.collections.iterator, and
// the element type is that instantiation's type argument. An 'Iterable<T>'
// value itself is iterable too. Reports and returns the error type otherwise.
Type* Analyzer::resolveIterableElement(Type* iterT, const SyntaxNode& diag) {
    StructInfo* si = iterT->structInfo;
    StructInfo* iterableInst = nullptr;
    if (isIterableInterface(si)) {
        iterableInst = si;
    } else {
        for (StructInfo* s = si; s && !iterableInst; s = s->baseInfo) {
            for (Type* it : s->implementedInterfaces) {
                if (it && isIterableInterface(it->structInfo)) {
                    iterableInst = it->structInfo;
                    break;
                }
            }
        }
    }
    if (!iterableInst) {
        errorAtNode(diag, "'" + iterT->toString() + "' is not iterable: it does not implement "
            "'Iterable' from '@std.collections.iterator'. Declare 'implements Iterable<T>' and provide "
            "'makeIterator() -> Iterator<T>' to use it in a for-in loop.");
        return typeCtx.getError();
    }
    if (iterableInst->typeArgs.empty() || !iterableInst->typeArgs[0]) {
        errorAtNode(diag, "Internal: 'Iterable' lost its element type argument");
        return typeCtx.getError();
    }
    return iterableInst->typeArgs[0];
}

void Analyzer::analyzeForEachStmt(const ast::ForEachStatement& stmt) {
    pushScope();
    Type* iterT = typeCtx.getError();
    if (auto it = stmt.iterable()) iterT = analyzeExpr(*it);
    Type* elemT = typeCtx.getError();
    if (!iterT->isError()) {
        if (iterT->isArray() && iterT->inner) {
            elemT = iterT->inner;
        } else if (iterT->isClass() && iterT->structInfo) {
            elemT = resolveIterableElement(iterT, stmt.node);
        } else {
            errorAtNode(stmt.node, "'for (... in ...)' requires an array or an iterable "
                "object, got '" + iterT->toString() + "'. A class is iterable when it "
                "implements 'Iterable<T>' from '@std.collections.iterator'.");
        }
    }
    Type* bindingT = elemT;
    if (auto tr = stmt.elementTypeRef()) {
        Type* declared = resolveTypeReference(*tr);
        if (!declared->isError() && !elemT->isError() && !declared->assignableFrom(elemT)) {
            errorAtNode(tr->node, "Loop variable of type '" + declared->toString() +
                "' cannot hold the '" + elemT->toString() + "' values this sequence yields. "
                "Declare it as '" + elemT->toString() + "' or a type '" + elemT->toString() +
                "' fits into.");
        }
        bindingT = declared;
    }
    auto name = stmt.elementNameText().value_or(std::u16string{});
    uint32_t namePos = stmt.elementNameToken()
        ? stmt.elementNameToken()->startOffset() : stmt.node.startOffset();
    Symbol* sym = makeSymbol(SymbolKind::Variable, name, bindingT, namePos);
    sym->isConst = stmt.isConst();
    currentScope->define(sym);
    analysis.setSymbol(stmt.node.greenNode(), sym);
    AssignmentFlow entry = snapshotAssignment();
    trackLocal(sym, /*assigned=*/true);
    breakFlows_.push_back({});
    loopDepth++;
    if (auto b = stmt.body()) {
        preClearLoopBodyWrites(b->node);
        analyzeBlock(*b);
    }
    loopDepth--;
    std::vector<AssignmentFlow> exits = std::move(breakFlows_.back());
    breakFlows_.pop_back();
    exits.push_back(entry);  // the sequence may be empty, so the body may not run
    restoreAssignment(joinAssignment(exits));
    popScope();
}

void Analyzer::analyzeBreakStmt(const ast::BreakStatement& stmt) {
    if (loopDepth == 0) errorAtNode(stmt.node, "'break' can only be used inside a loop.");
    // A break carries the current assignments to the loop's normal exit.
    if (assignmentActive_ && !flowTerminated_ && !breakFlows_.empty()) {
        breakFlows_.back().push_back(snapshotAssignment());
    }
    flowTerminated_ = true;
}

void Analyzer::analyzeContinueStmt(const ast::ContinueStatement& stmt) {
    if (loopDepth == 0) errorAtNode(stmt.node, "'continue' can only be used inside a loop.");
    flowTerminated_ = true;
}

void Analyzer::analyzeReturnStmt(const ast::ReturnStatement& stmt) {
    if (!currentFunction) {
        errorAtNode(stmt.node, "'return' can only be used inside a function body.");
        return;
    }
    if (currentFunction->isNoreturn) {
        errorAtNode(stmt.node, "A 'noreturn' function cannot use 'return'; it never returns "
            "normally. Throw, call 'panic', or call another 'noreturn' function instead.");
        if (auto value = stmt.value()) analyzeExpr(*value);
        flowTerminated_ = true;
        return;
    }
    // A return leaves the constructor normally, so every own non-defaultable field
    // must already be assigned on the path reaching it.
    if (ctorFieldClass_) checkConstructorFieldsAssigned(stmt.node);
    Type* expected = currentFunction->returnType;
    auto value = stmt.value();
    if (!value) {
        if (expected && !expected->isVoid()) {
            errorAtNode(stmt.node, "Function returns '" + expected->toString() +
                "', but 'return' has no value");
        }
        flowTerminated_ = true;
        return;
    }
    Type* actual = analyzeExprAdapt(*value, expected);
    if (expected && !expected->isVoid()) {
        if (!expected->assignableFrom(actual)) {
            errorAtNode(value->node, "Cannot return value of type '" + actual->toString() +
                "' from function returning '" + expected->toString() + "'");
        }
    } else if (expected && expected->isVoid()) {
        errorAtNode(value->node, "Function returns 'void' but 'return' has a value");
    }
    flowTerminated_ = true;
}

void Analyzer::analyzeExpressionStmt(const ast::ExpressionStatement& stmt) {
    unconditionalPosition_ = true;
    if (auto e = stmt.expression()) {
        analyzeExpr(*e);
        if (isNoreturnCall(*e)) flowTerminated_ = true;
    }
}

// =========================================================
// Expressions
// =========================================================

Type* Analyzer::analyzeExpr(const ast::Expression& expr) {
    Type* t = nullptr;
    if (auto lit = expr.asLiteral())    t = analyzeLiteral(*lit);
    else if (auto id = expr.asIdent())  t = analyzeIdent(*id);
    else if (auto th = expr.asThis())   t = analyzeThis(*th);
    else if (auto su = expr.asSuper())  t = analyzeSuper(*su);
    else if (auto b  = expr.asBinary()) t = analyzeBinary(*b);
    else if (auto p  = expr.asPrefix()) t = analyzePrefix(*p);
    else if (auto po = expr.asPostfix()) t = analyzePostfix(*po);
    else if (auto c  = expr.asCall())   t = analyzeCall(*c);
    else if (auto m  = expr.asMember()) t = analyzeMember(*m);
    else if (auto sm = expr.asSafeMember()) t = analyzeSafeMember(*sm);
    else if (auto su = expr.asSubscript()) t = analyzeSubscript(*su);
    else if (auto ss = expr.asSafeSubscript()) t = analyzeSafeSubscript(*ss);
    else if (auto ca = expr.asCast()) t = analyzeCast(*ca);
    else if (auto cc = expr.asCheckedCast()) t = analyzeCheckedCast(*cc);
    else if (auto tt = expr.asTypeTest()) t = analyzeTypeTest(*tt);
    else if (auto oa = expr.asOutArgument()) {
        errorAtNode(expr.node, "'out' can only be used when calling an external function.");
        t = typeCtx.getError();
    }
    else if (expr.asNamedArgument()) {
        errorAtNode(expr.node, "Named arguments can only be used when calling a function, "
            "method, or constructor.");
        t = typeCtx.getError();
    }
    else if (auto a  = expr.asAssign()) t = analyzeAssign(*a);
    else if (auto tn = expr.asTernary())t = analyzeTernary(*tn);
    else if (auto nc = expr.asNullCoalesce()) t = analyzeNullCoalesce(*nc);
    else if (auto nw = expr.asNew())    t = analyzeNew(*nw);
    else if (auto tr = expr.asTry())    t = analyzeTry(*tr);
    else if (auto pr = expr.asParen())  t = analyzeParen(*pr);
    else if (auto al = expr.asArrayLiteral()) t = analyzeArrayLiteral(*al);
    else if (auto sl = expr.asStructLiteral()) t = analyzeStructLiteral(*sl);
    else if (auto is = expr.asInterpString()) t = analyzeInterpString(*is);
    else if (auto sw = expr.asSwitch()) t = analyzeSwitchExpr(*sw);
    else                                t = typeCtx.getError();
    analysis.setType(expr.node.greenNode(), t);
    return t;
}

Type* Analyzer::analyzeInterpString(const ast::InterpStringExpression& expr) {
    Type* stringTy = typeCtx.getPrimitive(TypeKind::String);
    for (auto& hole : expr.holes()) {
        Type* ht = analyzeExpr(hole);
        if (ht->isError()) continue;
        // A bare type parameter or a generic struct is judged per instantiation during code
        // generation, the same way its equality and JSON emission are.
        if (ht->isTypeParam() || (ht->isStruct() && TypeContext::containsTypeParam(ht))) continue;
        if (ht->isInteger() || ht->isBool() || ht->isString() || ht->isEnum()) continue;
        if (ht->isStruct()) {
            // A struct that declares its own toString interpolates through that method and is
            // never serialized; without one it interpolates as its JSON form, which is valid
            // when every field is JSON-able.
            if (const MethodInfo* own = declaredToString(ht->structInfo)) {
                std::string issue = textFormIssue(ht->toString(), *own);
                if (!issue.empty()) errorAtNode(hole.node, issue);
                continue;
            }
            checkStructJsonable(ht, hole.node);
            continue;
        }
        errorAtNode(hole.node, "Cannot interpolate a value of type '" + ht->toString() +
            "'; only string, integer, bool, enum, and JSON-serializable struct values are supported here. "
            "Convert it with '.toString()' first.");
    }
    return stringTy;
}

Type* Analyzer::analyzeLiteral(const ast::LiteralExpression& expr) {
    switch (expr.literalKind()) {
        case SyntaxKind::IntLiteral:    return typeCtx.getPrimitive(TypeKind::Int);
        case SyntaxKind::LongLiteral:   return typeCtx.getPrimitive(TypeKind::Long);
        case SyntaxKind::FloatLiteral:  return typeCtx.getPrimitive(TypeKind::Float);
        case SyntaxKind::DoubleLiteral: return typeCtx.getPrimitive(TypeKind::Double);
        case SyntaxKind::StringLiteral: return typeCtx.getPrimitive(TypeKind::String);
        case SyntaxKind::CharLiteral:   return typeCtx.getPrimitive(TypeKind::Char);
        case SyntaxKind::KwTrue:
        case SyntaxKind::KwFalse:       return typeCtx.getPrimitive(TypeKind::Bool);
        case SyntaxKind::KwNull:        return typeCtx.getNull();
        default:                        return typeCtx.getError();
    }
}

Type* Analyzer::analyzeIdent(const ast::IdentExpression& expr) {
    auto name = expr.nameText();
    if (!name) return typeCtx.getError();
    Symbol* sym = currentScope ? currentScope->lookup(*name) : nullptr;
    if (!sym) {
        errorAtNode(expr.node, "Undefined name '" + asciiOf(*name) + "'");
        return typeCtx.getError();
    }
    analysis.setSymbol(expr.node.greenNode(), sym);
    if (sym->kind == SymbolKind::Function) return typeCtx.getError();
    if (sym->kind == SymbolKind::Namespace) return typeCtx.getError();
    checkDefiniteAssignment(expr, sym);
    if (currentScope) {
        if (Type* narrowed = currentScope->lookupNarrowedType(NarrowingPath{sym, {}})) {
            return narrowed;
        }
    }
    return sym->type ? sym->type : typeCtx.getError();
}

Type* Analyzer::analyzeThis(const ast::ThisExpression& expr) {
    if (!currentThis) {
        errorAtNode(expr.node, "'this' is only valid inside a method");
        return typeCtx.getError();
    }
    analysis.setSymbol(expr.node.greenNode(), currentThis);
    return currentThis->type ? currentThis->type : typeCtx.getError();
}

Type* Analyzer::analyzeSuper(const ast::SuperExpression& expr) {
    if (!currentThis || !currentThis->type || !currentThis->type->structInfo) {
        errorAtNode(expr.node, "'super' is only valid inside a method");
        return typeCtx.getError();
    }
    StructInfo* cls = currentThis->type->structInfo;
    if (!cls->baseInfo) {
        errorAtNode(expr.node, "'super' cannot be used in '" + asciiOf(cls->name) +
            "' because it has no base class");
        return typeCtx.getError();
    }
    analysis.setSymbol(expr.node.greenNode(), currentThis);
    Type* baseT = typeCtx.lookupClass(modulePath_, cls->baseInfo->name);
    return baseT ? baseT : typeCtx.getError();
}

bool Analyzer::isLocalClass(StructInfo* definingClass) {
    if (!definingClass) return false;
    return declarationAuthority(definingClass)->modulePath == modulePath_;
}

bool Analyzer::isMemberAccessAllowed(Visibility visibility, StructInfo* definingClass) {
    if (visibility == Visibility::Export) return true;
    if (visibility == Visibility::Public) {
        const StructInfo* defining = declarationAuthority(definingClass);
        return !defining || defining->packagePrefix == packagePrefix_;
    }
    StructInfo* current = (currentThis && currentThis->type) ? currentThis->type->structInfo : nullptr;
    if (visibility == Visibility::Private) {
        return current && definingClass &&
               declarationAuthority(current) == declarationAuthority(definingClass);
    }
    // Protected: visible in a subclass method, or anywhere in the declaring class's own file.
    if (current && definingClass && current->isSubclassOf(definingClass)) return true;
    return isLocalClass(definingClass);
}

void Analyzer::checkMemberAccess(const SyntaxNode& diagNode, const std::u16string& memberName,
                                 Visibility visibility, StructInfo* definingClass) {
    if (isMemberAccessAllowed(visibility, definingClass)) return;
    std::string owner = definingClass ? asciiOf(definingClass->name) : std::string("its class");
    if (visibility == Visibility::Private) {
        errorAtNode(diagNode, "'" + asciiOf(memberName) + "' is private to class '" + owner +
            "' and can only be accessed from inside '" + owner + "'");
    } else if (visibility == Visibility::Public) {
        const StructInfo* defining = declarationAuthority(definingClass);
        std::string packageName = defining && !defining->packagePrefix.empty()
            ? "package '" + asciiOf(defining->packagePrefix) + "'" : "its own package";
        errorAtNode(diagNode, "'" + asciiOf(memberName) + "' of '" + owner +
            "' is public inside " + packageName + " but not exported. Mark it 'export' to use "
            "it from another package.");
    } else {
        errorAtNode(diagNode, "'" + asciiOf(memberName) + "' is protected to class '" + owner +
            "' and can only be accessed from inside '" + owner +
            "', a subclass of it, or the file where '" + owner + "' is declared");
    }
}

// The least-visible declared type mentioned anywhere inside `t`: arrays and
// optionals look through to their element, an instantiation covers its template
// and every type argument, and type parameters mention nothing themselves.
static void collectLeastVisible(const Type* t, const StructInfo*& worst) {
    if (!t) return;
    if (t->isOptional() || t->isArray()) {
        collectLeastVisible(t->inner, worst);
        return;
    }
    if (t->isTypeParam()) return;
    const StructInfo* si = t->structInfo;
    if (!si) return;
    const StructInfo* authority = declarationAuthority(si);
    if (!worst || visibilityTier(authority->visibility) < visibilityTier(worst->visibility)) {
        worst = authority;
    }
    for (const Type* arg : si->typeArgs) collectLeastVisible(arg, worst);
}

static std::string invisibleTypePhrase(const StructInfo* si) {
    if (si->visibility == Visibility::Private) {
        return "private to module '" + asciiOf(si->modulePath) + "'";
    }
    std::string packageName = si->packagePrefix.empty()
        ? std::string("its own package") : "package '" + asciiOf(si->packagePrefix) + "'";
    return "only public inside " + packageName;
}

// The widest scope from which a class can legally be subclassed, which is the
// leakage floor of its protected members: every subclasser must be able to name
// the types those members mention.
static Visibility protectedMemberReach(const StructInfo* si) {
    if (!si || si->isFinal || si->isSealed) return Visibility::Protected;
    return si->visibility;
}

void Analyzer::checkMentionedTypeValue(Type* t, const SyntaxNode& diagNode,
                                       Visibility declVisibility, const std::string& declPhrase,
                                       const std::string& role, StructInfo* protectedOwner) {
    if (!t) return;
    const StructInfo* worst = nullptr;
    collectLeastVisible(t, worst);
    if (!worst || visibilityTier(worst->visibility) >= visibilityTier(declVisibility)) return;
    std::string mentioned = asciiOf(worst->name);
    std::string needed = visibilityWord(declVisibility);
    if (protectedOwner) {
        std::string owner = asciiOf(protectedOwner->name);
        std::string reach = declVisibility == Visibility::Export
            ? "subclasses in other packages" : "subclasses anywhere in the package";
        errorAtNode(diagNode, declPhrase + " is protected, and '" + owner + "' is an open " +
            needed + " class whose " + reach + " can reach it, but its " + role +
            " mentions '" + mentioned + "', which is " + invisibleTypePhrase(worst) +
            ". Mark '" + mentioned + "' as '" + needed + "', lower '" + owner +
            "', or mark '" + owner + "' 'final'.");
        return;
    }
    errorAtNode(diagNode, declPhrase + " is visible as '" + needed +
        "' but its " + role + " mentions '" + mentioned + "', which is " +
        invisibleTypePhrase(worst) + ". Mark '" + mentioned + "' as '" + needed +
        "', or lower the declaration.");
}

void Analyzer::checkMentionedType(const ast::TypeReference& tr, Visibility declVisibility,
                                  const std::string& declPhrase, const std::string& role,
                                  StructInfo* protectedOwner) {
    checkMentionedTypeValue(analysis.typeOf(tr.node.greenNode()), tr.node, declVisibility,
                            declPhrase, role, protectedOwner);
}

void Analyzer::checkTypeParamBoundsVisibility(const std::vector<ast::TypeParam>& params,
                                              Visibility declVisibility,
                                              const std::string& declPhrase,
                                              StructInfo* protectedOwner) {
    for (auto& tp : params) {
        for (auto& bound : tp.bounds()) {
            checkMentionedType(bound, declVisibility, declPhrase, "type-parameter bound",
                               protectedOwner);
        }
    }
}

void Analyzer::checkCallableSignatureVisibility(const ast::FuncDecl& fn,
                                                Visibility declVisibility,
                                                const std::string& declPhrase,
                                                StructInfo* protectedOwner) {
    if (visibilityTier(declVisibility) == 0) return;
    const ResolutionInfo* info = analysis.find(fn.node.greenNode());
    Symbol* sym = info ? info->resolvedSymbol : nullptr;
    auto params = fn.parameters();
    for (size_t i = 0; i < params.size(); ++i) {
        if (auto tr = params[i].typeReference()) {
            checkMentionedType(*tr, declVisibility, declPhrase, "parameter type",
                               protectedOwner);
        } else if (params[i].isThisField() && sym && i < sym->paramTypes.size()) {
            // A `this.field` shorthand parameter mentions the field's type.
            checkMentionedTypeValue(sym->paramTypes[i], params[i].node, declVisibility,
                                    declPhrase, "parameter type", protectedOwner);
        }
    }
    if (auto rt = fn.returnType()) {
        if (auto tr = rt->typeReference()) {
            checkMentionedType(*tr, declVisibility, declPhrase, "return type", protectedOwner);
        }
    }
    for (auto& tr : fn.declaredThrowsTypes()) {
        checkMentionedType(tr, declVisibility, declPhrase, "thrown type", protectedOwner);
    }
    checkTypeParamBoundsVisibility(fn.typeParams(), declVisibility, declPhrase, protectedOwner);
}

void Analyzer::checkSignatureVisibility() {
    if (!astRoot) return;
    auto& sf = *astRoot;

    for (auto& fn : sf.functions()) {
        const ResolutionInfo* info = analysis.find(fn.node.greenNode());
        Symbol* sym = info ? info->resolvedSymbol : nullptr;
        if (!sym) continue;
        checkCallableSignatureVisibility(fn, sym->visibility,
            "Function '" + asciiOf(sym->name) + "'");
    }

    auto methodVisibilityOf = [](StructInfo* si, const ast::FuncDecl& m) {
        for (auto& mi : si->methods) {
            if (mi.declaration == m.node.greenNode()) return mi.visibility;
        }
        return Visibility::Private;
    };
    auto fieldVisibilityOf = [](StructInfo* si, const ast::FieldDecl& f) {
        for (auto& fi : si->fields) {
            if (fi.declaration == f.node.greenNode()) return fi.visibility;
        }
        return Visibility::Private;
    };
    auto checkMembers = [&](StructInfo* si, const std::string& kindWord, bool isClass,
                            const std::vector<ast::FieldDecl>& fields,
                            const std::vector<ast::FuncDecl>& methods) {
        std::string owner = kindWord + " '" + asciiOf(si->name) + "'";
        auto effectiveReach = [&](Visibility v, StructInfo*& protectedOwner) {
            protectedOwner = nullptr;
            if (v != Visibility::Protected || !isClass) return v;
            Visibility reach = protectedMemberReach(si);
            if (visibilityTier(reach) > 0) protectedOwner = si;
            return reach;
        };
        for (auto& f : fields) {
            StructInfo* protectedOwner = nullptr;
            Visibility v = effectiveReach(fieldVisibilityOf(si, f), protectedOwner);
            if (visibilityTier(v) == 0) continue;
            if (auto tr = f.typeReference()) {
                checkMentionedType(*tr, v, "Field '" +
                    asciiOf(f.nameText().value_or(std::u16string{})) + "' of " + owner,
                    "type", protectedOwner);
            }
        }
        for (auto& m : methods) {
            StructInfo* protectedOwner = nullptr;
            Visibility v = effectiveReach(methodVisibilityOf(si, m), protectedOwner);
            std::string memberWord = m.isConstructor() ? "Constructor" : "Method '" +
                asciiOf(m.nameText().value_or(std::u16string{})) + "'";
            checkCallableSignatureVisibility(m, v, memberWord + " of " + owner,
                                             protectedOwner);
        }
    };

    for (auto& sd : sf.structs()) {
        Type* t = analysis.typeOf(sd.node.greenNode());
        if (!t || !t->structInfo) continue;
        StructInfo* si = t->structInfo;
        std::string declPhrase = "Struct '" + asciiOf(si->name) + "'";
        checkTypeParamBoundsVisibility(sd.typeParams(), si->visibility, declPhrase);
        checkMembers(si, "struct", false, sd.fields(), sd.methods());
    }

    for (auto& cd : sf.classes()) {
        Type* t = analysis.typeOf(cd.node.greenNode());
        if (!t || !t->structInfo) continue;
        StructInfo* si = t->structInfo;
        std::string declPhrase = "Class '" + asciiOf(si->name) + "'";
        if (si->baseInfo && visibilityTier(si->visibility) > 0) {
            const StructInfo* baseAuthority = declarationAuthority(si->baseInfo);
            if (visibilityTier(baseAuthority->visibility) < visibilityTier(si->visibility)) {
                errorAtNode(cd.baseClassToken().value_or(cd.node), declPhrase +
                    " is visible as '" + visibilityWord(si->visibility) +
                    "' but its base class '" + asciiOf(baseAuthority->name) + "' is " +
                    invisibleTypePhrase(baseAuthority) + ". Mark '" +
                    asciiOf(baseAuthority->name) + "' as '" + visibilityWord(si->visibility) +
                    "', or lower the declaration.");
            }
            for (auto& tr : cd.baseTypeArguments()) {
                checkMentionedType(tr, si->visibility, declPhrase, "base class type argument");
            }
        }
        if (visibilityTier(si->visibility) > 0) {
            for (auto& tr : cd.implementedInterfaceRefs()) {
                checkMentionedType(tr, si->visibility, declPhrase, "implemented interface");
            }
        }
        checkTypeParamBoundsVisibility(cd.typeParams(), si->visibility, declPhrase);
        checkMembers(si, "class", true, cd.fields(), cd.methods());
    }

    for (auto& id : sf.interfaces()) {
        Type* t = analysis.typeOf(id.node.greenNode());
        if (!t || !t->structInfo) continue;
        StructInfo* si = t->structInfo;
        std::string declPhrase = "Interface '" + asciiOf(si->name) + "'";
        checkTypeParamBoundsVisibility(id.typeParams(), si->visibility, declPhrase);
        std::string owner = "interface '" + asciiOf(si->name) + "'";
        for (auto& m : id.methods()) {
            std::string memberWord = "Method '" +
                asciiOf(m.nameText().value_or(std::u16string{})) + "'";
            checkCallableSignatureVisibility(m, si->visibility, memberWord + " of " + owner);
        }
    }
}

// Constructor calls run through the same accessibility rules as any other member;
// only the wording is constructor-specific.
void Analyzer::checkConstructorAccess(const SyntaxNode& diagNode, StructInfo* owner,
                                      const MethodInfo& constructor) {
    StructInfo* defining = constructor.definingClass ? constructor.definingClass : owner;
    if (isMemberAccessAllowed(constructor.visibility, defining)) return;
    std::string name = asciiOf(owner ? owner->name : std::u16string(u"?"));
    const StructInfo* authority = declarationAuthority(defining);
    if (constructor.visibility == Visibility::Private) {
        std::string fix = authority && authority->packagePrefix == packagePrefix_
            ? "public" : "export";
        errorAtNode(diagNode, "The constructor of '" + name + "' is private and can only be "
            "called from inside '" + name + "'. Mark it '" + fix + "' to construct '" + name +
            "' here.");
    } else if (constructor.visibility == Visibility::Public) {
        std::string packageName = authority && !authority->packagePrefix.empty()
            ? "package '" + asciiOf(authority->packagePrefix) + "'" : "its own package";
        errorAtNode(diagNode, "The constructor of '" + name + "' is public inside " +
            packageName + " but not exported. Mark it 'export' to construct '" + name +
            "' from another package.");
    } else {
        errorAtNode(diagNode, "The constructor of '" + name + "' is protected; only '" + name +
            "', its subclasses, and the file that declares '" + name + "' can call it.");
    }
}

Type* Analyzer::analyzeBinary(const ast::BinaryExpression& expr) {
    auto left = expr.left();
    auto right = expr.right();
    if (!left || !right) return typeCtx.getError();

    auto opTok = expr.operatorToken();
    SyntaxKind op = opTok ? opTok->kind() : SyntaxKind::Invalid;
    std::string opText = opTok ? asciiOf(opTok->tokenText()) : "";

    // `&&` and `||` evaluate the right side only when the left side did not
    // already decide the result, so a null check on the left narrows the right:
    // `x != null && x.ready()` and `x == null || x.ready()` are both safe.
    if (op == SyntaxKind::AmpAmp || op == SyntaxKind::PipePipe) {
        Type* l = analyzeExpr(*left);
        std::vector<NullCheckInfo> narrowings;
        collectNarrowings(*left, /*conditionHolds=*/op == SyntaxKind::AmpAmp, narrowings);
        pushScope();
        for (const auto& info : narrowings) {
            currentScope->narrowedTypes[info.key] = info.narrowedT;
        }
        bool prevUncond = unconditionalPosition_;
        unconditionalPosition_ = false;
        Type* r = analyzeExpr(*right);
        unconditionalPosition_ = prevUncond;
        popScope();
        if (l->isError() || r->isError()) return typeCtx.getError();
        if (!l->isBool() || !r->isBool()) {
            errorAtNode(expr.node, "'" + opText + "' needs 'bool' on both sides, got '" +
                l->toString() + "' and '" + r->toString() + "'.");
        }
        return typeCtx.getPrimitive(TypeKind::Bool);
    }

    Type* l = analyzeExpr(*left);
    Type* r = analyzeExpr(*right);
    if (l->isError() || r->isError()) return typeCtx.getError();

    // Adapt polymorphic integer literals to the other operand's type when one
    // side is a non-literal concrete numeric type. Re-read after adaptation.
    auto tryAdaptOperands = [&]() {
        tryAdaptIntegerLiteral(*left, r);
        tryAdaptIntegerLiteral(*right, l);
        Type* lu = analysis.typeOf(left->node.greenNode());
        Type* ru = analysis.typeOf(right->node.greenNode());
        if (lu) l = lu;
        if (ru) r = ru;
    };

    switch (op) {
        case SyntaxKind::Plus:
        case SyntaxKind::Minus:
        case SyntaxKind::Star:
        case SyntaxKind::Slash:
        case SyntaxKind::Percent: {
            if (op == SyntaxKind::Plus && (l->isString() || r->isString())) {
                // String concatenation. The non-string operand is converted to
                // text implicitly, the same way '.toString()' would.
                auto stringable = [](Type* t) {
                    return t->isString() || t->isInteger() || t->isBool();
                };
                if (stringable(l) && stringable(r)) {
                    return typeCtx.getPrimitive(TypeKind::String);
                }
                Type* other = l->isString() ? r : l;
                errorAtNode(expr.node, "Cannot concatenate a value of type '" + other->toString() +
                    "' onto a string; only string, integer, and bool values are supported.");
                return typeCtx.getError();
            }
            if (!l->isNumeric() || !r->isNumeric()) {
                errorAtNode(expr.node, "'" + opText + "' needs numbers on both sides, got '" +
                    l->toString() + "' and '" + r->toString() + "'.");
                return typeCtx.getError();
            }
            tryAdaptOperands();
            Type* common = numericCommonType(l, r);
            if (!common) {
                errorAtNode(expr.node, "'" + opText + "' needs the same numeric type on both "
                    "sides, got '" + l->toString() + "' and '" + r->toString() +
                    "'; convert one side with 'as'.");
                return typeCtx.getError();
            }
            return common;
        }

        case SyntaxKind::EqEq:
        case SyntaxKind::NotEq: {
            tryAdaptOperands();
            // A comparison against `null` is a presence test on the declared
            // storage type, so it stays legal on a binding declared optional
            // even where narrowing already proved the value non-null.
            if (r->isNull())      l = presenceOperandType(*left, l);
            else if (l->isNull()) r = presenceOperandType(*right, r);
            if (!l->assignableFrom(r) && !r->assignableFrom(l)) {
                errorAtNode(expr.node, "Cannot compare '" + l->toString() + "' and '" + r->toString() + "'");
            } else if (l->isStruct() && r->isStruct() && l->structInfo && l->equals(r)) {
                // Same-type struct '==' lowers to a memberwise compare; a field
                // whose type has no '==' of its own makes the whole compare an error.
                checkStructEquatable(l, expr.node);
            }
            return typeCtx.getPrimitive(TypeKind::Bool);
        }

        case SyntaxKind::Lt:
        case SyntaxKind::Gt:
        case SyntaxKind::LtEq:
        case SyntaxKind::GtEq: {
            if (!l->isNumeric() || !r->isNumeric()) {
                errorAtNode(expr.node, "'" + opText + "' compares numbers, got '" +
                    l->toString() + "' and '" + r->toString() + "'.");
                return typeCtx.getPrimitive(TypeKind::Bool);
            }
            tryAdaptOperands();
            if (!numericCommonType(l, r)) {
                errorAtNode(expr.node, "'" + opText + "' needs the same numeric type on both "
                    "sides, got '" + l->toString() + "' and '" + r->toString() +
                    "'; convert one side with 'as'.");
            }
            return typeCtx.getPrimitive(TypeKind::Bool);
        }

        case SyntaxKind::Amp:
        case SyntaxKind::Pipe:
        case SyntaxKind::Caret:
        case SyntaxKind::LtLt:
        case SyntaxKind::GtGt:
        case SyntaxKind::GtGtGt:
            if (!l->isInteger() || !r->isInteger() || !l->equals(r)) {
                errorAtNode(expr.node, "Bitwise operator requires matching integer operands, got '" +
                    l->toString() + "' and '" + r->toString() + "'");
            }
            return l;

        default:
            errorAtNode(expr.node, "Unsupported binary operator");
            return typeCtx.getError();
    }
}

Type* Analyzer::analyzePrefix(const ast::PrefixExpression& expr) {
    auto operand = expr.operand();
    if (!operand) return typeCtx.getError();
    Type* t = analyzeExpr(*operand);
    if (t->isError()) return typeCtx.getError();
    auto opTok = expr.operatorToken();
    if (!opTok) return typeCtx.getError();
    switch (opTok->kind()) {
        case SyntaxKind::Minus:
            if (!t->isNumeric()) {
                errorAtNode(expr.node, "Unary '-' requires numeric, got '" + t->toString() + "'");
                return typeCtx.getError();
            }
            return t;
        case SyntaxKind::Bang:
            if (!t->isBool()) {
                errorAtNode(expr.node, "Unary '!' requires bool, got '" + t->toString() + "'");
                return typeCtx.getError();
            }
            return t;
        case SyntaxKind::PlusPlus:
        case SyntaxKind::MinusMinus:
            return analyzeIncDec(expr.node, *operand, t,
                                 opTok->kind() == SyntaxKind::PlusPlus, /*isPrefix=*/true);
        default:
            errorAtNode(expr.node, "Unsupported unary operator");
            return typeCtx.getError();
    }
}

Type* Analyzer::analyzePostfix(const ast::PostfixExpression& expr) {
    auto operand = expr.operand();
    if (!operand) return typeCtx.getError();
    Type* t = analyzeExpr(*operand);
    if (t->isError()) return typeCtx.getError();
    auto opTok = expr.operatorToken();
    if (!opTok) return typeCtx.getError();
    return analyzeIncDec(expr.node, *operand, t,
                         opTok->kind() == SyntaxKind::PlusPlus, /*isPrefix=*/false);
}

Type* Analyzer::analyzeIncDec(const SyntaxNode& exprNode, const ast::Expression& operand,
                              Type* operandT, bool isIncrement, bool isPrefix) {
    const char* op = isIncrement ? "++" : "--";
    std::string example = isPrefix ? std::string(op) + "count" : std::string("count") + op;
    if (!operandT->isNumeric()) {
        errorAtNode(exprNode, std::string("The '") + op + "' operator works only on numbers, so it "
            "cannot be applied to a value of type '" + operandT->toString() + "'; use it on an "
            "integer or floating-point variable, for example '" + example + "'.");
        return operandT;
    }
    if (!isLValue(operand)) {
        errorAtNode(exprNode, std::string("The '") + op + "' operator needs a variable to change, "
            "for example '" + example + "'; it cannot be applied to something that is not a "
            "variable, field, or array element.");
        return operandT;
    }
    if (auto id = operand.asIdent()) {
        if (auto* info = analysis.find(id->node.greenNode())) {
            if (Symbol* sym = info->resolvedSymbol; sym && sym->isConst) {
                errorAtNode(exprNode, std::string("The '") + op + "' operator cannot change '" +
                    asciiOf(sym->name) + "' because it is declared as constant; use 'let' instead "
                    "of 'const' if it needs to change.");
                return operandT;
            }
        }
    }
    invalidateNarrowingsForWrite(operand);
    return operandT;
}

static size_t requiredArgCount(Symbol* sym) {
    if (!sym->funcDeclCst) return sym->paramTypes.size();
    auto fnNode = SyntaxNode::makeRoot(sym->funcDeclCst);
    auto fn = ast::FuncDecl::cast(*fnNode);
    if (!fn) return sym->paramTypes.size();
    size_t required = 0;
    for (auto& p : fn->parameters()) {
        if (p.defaultValue()) break;
        required++;
    }
    return required;
}

// =========================================================
// Overload resolution
// =========================================================

namespace {

constexpr int kRankNone = 0;
constexpr int kRankLiteral = 1;   // viable only because the literal's value fits
constexpr int kRankConvert = 2;   // numeric widening or another implicit conversion
constexpr int kRankExact = 3;

std::vector<Symbol*> overloadChainOf(Symbol* head) {
    std::vector<Symbol*> chain;
    for (Symbol* s = head; s; s = s->nextOverload) chain.push_back(s);
    return chain;
}

// How well an argument fits a parameter. Untyped integer and char literals
// adapt to any integer type their value fits, as they do for assignments.
int conversionRank(const ast::Expression& arg, Type* argT, Type* paramT) {
    if (!argT || !paramT) return kRankNone;
    if (argT->isError() || paramT->isError()) return kRankExact;
    if (argT->equals(paramT)) return kRankExact;
    if (paramT->isInteger()) {
        if (const ast::LiteralExpression* lit = asIntLiteralChild(arg)) {
            auto tok = lit->token();
            uint64_t magnitude = 0;
            if (tok && parseIntegerLiteralMagnitude(std::u16string(tok->tokenText()), magnitude) &&
                literalFitsTarget(literalIsNegative(arg), magnitude, paramT)) {
                return argT->widensTo(paramT) ? kRankConvert : kRankLiteral;
            }
        }
        if (auto lit = arg.asLiteral(); lit && lit->literalKind() == SyntaxKind::CharLiteral) {
            if (auto tok = lit->token()) {
                uint32_t cp = parseCharLiteralCodepoint(tok->tokenText());
                if (literalFitsTarget(/*negative*/ false, cp, paramT)) {
                    return argT->widensTo(paramT) ? kRankConvert : kRankLiteral;
                }
            }
        }
    }
    if (paramT->assignableFrom(argT)) return kRankConvert;
    return kRankNone;
}

// Parameter names and default-value presence, read from the declaration CST.
void parameterInfoOf(Symbol* sym, std::vector<std::u16string>& names, std::vector<bool>& defaults) {
    names.clear();
    defaults.clear();
    if (!sym->funcDeclCst) return;
    auto fnNode = SyntaxNode::makeRoot(sym->funcDeclCst);
    auto fn = ast::FuncDecl::cast(*fnNode);
    if (!fn) return;
    for (auto& p : fn->parameters()) {
        names.push_back(p.nameText().value_or(std::u16string{}));
        defaults.push_back(p.defaultValue().has_value());
    }
}

}  // namespace

Analyzer::CallShape Analyzer::analyzeCallShape(const std::vector<ast::Expression>& args) {
    CallShape shape;
    for (auto& a : args) {
        if (auto na = a.asNamedArgument()) {
            auto argName = na->nameText();
            auto value = na->value();
            if (!argName || !value) {
                shape.malformed = true;
                shape.hasErrorArg = true;
                continue;
            }
            for (auto& prev : shape.named) {
                if (prev.name == *argName) {
                    errorAtNode(a.node, "Duplicate named argument '" + asciiOf(*argName) + "'.");
                    shape.malformed = true;
                    break;
                }
            }
            Type* t = analyzeExpr(*value);
            analysis.setType(a.node.greenNode(), t);
            if (t->isError()) shape.hasErrorArg = true;
            shape.named.push_back({*argName, *value, a.node, t});
        } else {
            if (!shape.named.empty() && !shape.malformed) {
                errorAtNode(a.node, "Positional arguments must come before named arguments.");
                shape.malformed = true;
            }
            Type* t = analyzeExpr(a);
            if (t->isError()) shape.hasErrorArg = true;
            shape.positional.push_back(a);
            shape.positionalTypes.push_back(t);
        }
    }
    return shape;
}

// Binds every source-order argument to a parameter index. False when the shape
// cannot call `sym`; `failure` then explains why for single-candidate reporting.
bool Analyzer::mapCallArguments(Symbol* sym, const CallShape& shape, std::vector<int>& mapping,
                                int& defaultedCount, std::string& failure,
                                const std::string& kindWord, const std::string& displayName) {
    size_t nParams = sym->paramTypes.size();
    size_t nPos = shape.positional.size();
    size_t total = nPos + shape.named.size();
    size_t req = requiredArgCount(sym);
    if (nPos > nParams) {
        failure = kindWord + " '" + displayName + "' expects " + std::to_string(req) +
            (req == nParams ? "" : "-" + std::to_string(nParams)) +
            " argument(s), got " + std::to_string(total);
        return false;
    }

    std::vector<std::u16string> names;
    std::vector<bool> defaults;
    parameterInfoOf(sym, names, defaults);

    std::vector<bool> bound(nParams, false);
    mapping.clear();
    mapping.reserve(total);
    for (size_t i = 0; i < nPos; ++i) {
        bound[i] = true;
        mapping.push_back(static_cast<int>(i));
    }
    for (auto& na : shape.named) {
        int idx = -1;
        for (size_t j = 0; j < names.size(); ++j) {
            if (names[j] == na.name) { idx = static_cast<int>(j); break; }
        }
        if (idx < 0 || idx >= static_cast<int>(nParams)) {
            failure = kindWord + " '" + displayName + "' has no parameter named '" +
                asciiOf(na.name) + "'";
            return false;
        }
        if (bound[idx]) {
            failure = kindWord + " '" + displayName + "': parameter '" + asciiOf(na.name) +
                "' is already bound by a positional argument";
            return false;
        }
        bound[idx] = true;
        mapping.push_back(idx);
    }
    defaultedCount = 0;
    for (size_t j = 0; j < nParams; ++j) {
        if (bound[j]) continue;
        if (j < defaults.size() && defaults[j]) { defaultedCount++; continue; }
        std::string pname = (j < names.size() && !names[j].empty())
            ? "'" + asciiOf(names[j]) + "'" : std::to_string(j + 1);
        failure = kindWord + " '" + displayName + "' is missing an argument for parameter " + pname;
        return false;
    }
    return true;
}

// Picks the best candidate: filter by argument shape and viability, rank
// argument matches (exact beats widening beats literal fitting), prefer
// accessible candidates, and report no-match/ambiguity errors here.
Analyzer::OverloadChoice Analyzer::resolveOverloadedCall(
        const std::vector<OverloadCandidate>& candidates, const CallShape& shape,
        const SyntaxNode& diagNode, const std::string& displayName,
        const std::string& kindWord) {
    OverloadChoice out;
    if (shape.malformed) {
        out.failed = true;
        return out;
    }

    struct Viable {
        const OverloadCandidate* cand;
        std::vector<int> mapping;
        std::vector<int> ranks;
        int defaulted = 0;
    };
    std::vector<Viable> viable;
    std::string singleFailure;
    size_t sourceCount = shape.positional.size() + shape.named.size();
    for (auto& c : candidates) {
        std::vector<int> mapping;
        int defaulted = 0;
        std::string failure;
        if (!mapCallArguments(c.symbol, shape, mapping, defaulted, failure, kindWord, displayName)) {
            if (candidates.size() == 1) singleFailure = failure;
            continue;
        }
        std::vector<int> ranks;
        ranks.reserve(sourceCount);
        bool ok = true;
        for (size_t i = 0; i < sourceCount && ok; ++i) {
            const ast::Expression& e = i < shape.positional.size()
                ? shape.positional[i] : shape.named[i - shape.positional.size()].value;
            Type* argT = i < shape.positional.size()
                ? shape.positionalTypes[i] : shape.named[i - shape.positional.size()].type;
            Type* paramT = c.symbol->paramTypes[mapping[i]];
            int rank = conversionRank(e, argT, paramT);
            // A single candidate is checked argument by argument afterwards,
            // in the same style as a non-overloaded call.
            if (rank == kRankNone && candidates.size() > 1 && !shape.hasErrorArg) ok = false;
            ranks.push_back(rank);
        }
        if (!ok) continue;
        viable.push_back({&c, std::move(mapping), std::move(ranks), defaulted});
    }

    if (viable.empty()) {
        if (candidates.size() == 1 && !singleFailure.empty()) {
            errorAtNode(diagNode, singleFailure);
        } else {
            std::string list;
            for (size_t i = 0; i < candidates.size(); ++i) {
                if (i) list += ", ";
                list += "'" + signatureOf(candidates[i].symbol) + "'";
            }
            errorAtNode(diagNode, "No overload of '" + displayName +
                "' matches this call. Candidates: " + list + ".");
        }
        out.failed = true;
        return out;
    }

    // Inaccessible candidates lose to accessible ones; they only win (and then
    // report a visibility error at the call site) when nothing accessible matches.
    bool anyAccessible = false;
    for (auto& v : viable) anyAccessible = anyAccessible || v.cand->accessible;
    std::vector<const Viable*> pool;
    for (auto& v : viable) {
        if (v.cand->accessible == anyAccessible) pool.push_back(&v);
    }

    auto betterThan = [](const Viable& a, const Viable& b) {
        bool atLeastAsGood = true;
        bool strictlyBetter = false;
        for (size_t i = 0; i < a.ranks.size(); ++i) {
            if (a.ranks[i] < b.ranks[i]) atLeastAsGood = false;
            if (a.ranks[i] > b.ranks[i]) strictlyBetter = true;
        }
        if (!atLeastAsGood) return false;
        if (strictlyBetter) return true;
        return a.defaulted < b.defaulted;  // equal ranks: fewer defaulted parameters wins
    };
    std::vector<const Viable*> best;
    for (const Viable* v : pool) {
        bool dominated = false;
        for (const Viable* o : pool) {
            if (o != v && betterThan(*o, *v)) { dominated = true; break; }
        }
        if (!dominated) best.push_back(v);
    }
    if (best.empty()) best.push_back(pool.front());
    if (best.size() > 1 && !shape.hasErrorArg) {
        std::string list;
        for (size_t i = 0; i < best.size(); ++i) {
            if (i) list += ", ";
            list += "'" + signatureOf(best[i]->cand->symbol) + "'";
        }
        errorAtNode(diagNode, "Call to '" + displayName +
            "' is ambiguous. Candidates: " + list + ".");
        out.failed = true;
        return out;
    }

    const Viable* winner = best.front();
    out.symbol = winner->cand->symbol;
    out.method = winner->cand->method;
    out.argParamIndex = winner->mapping;
    out.accessible = winner->cand->accessible;
    return out;
}

// Final argument checking against the chosen overload: adapt literals to the
// parameter types and verify assignability, mirroring the non-overloaded path.
Type* Analyzer::checkResolvedCallArguments(const CallShape& shape, const OverloadChoice& choice,
                                           const GreenElement* callNode) {
    Symbol* sym = choice.symbol;
    auto checkOne = [&](const ast::Expression& valueExpr, Type* argT, int paramIdx,
                        const std::string& argLabel, const SyntaxNode& diagNode) {
        if (paramIdx < 0 || paramIdx >= static_cast<int>(sym->paramTypes.size())) return;
        Type* paramT = sym->paramTypes[paramIdx];
        if (!paramT || paramT->isError() || !argT || argT->isError()) return;
        tryAdaptIntegerLiteral(valueExpr, paramT);
        tryAdaptCharLiteral(valueExpr, paramT);
        Type* updated = analysis.typeOf(valueExpr.node.greenNode());
        Type* finalT = updated ? updated : argT;
        if (!paramT->assignableFrom(finalT)) {
            errorAtNode(diagNode, "Argument " + argLabel + ": expected '" +
                paramT->toString() + "', got '" + finalT->toString() + "'");
        }
    };
    for (size_t i = 0; i < shape.positional.size() && i < choice.argParamIndex.size(); ++i) {
        checkOne(shape.positional[i], shape.positionalTypes[i], choice.argParamIndex[i],
                 std::to_string(i + 1), shape.positional[i].node);
    }
    for (size_t k = 0; k < shape.named.size(); ++k) {
        size_t i = shape.positional.size() + k;
        if (i >= choice.argParamIndex.size()) break;
        checkOne(shape.named[k].value, shape.named[k].type, choice.argParamIndex[i],
                 "'" + asciiOf(shape.named[k].name) + "'", shape.named[k].node);
    }
    if (!shape.named.empty() && callNode) {
        analysis.setCallArgOrder(callNode, choice.argParamIndex);
    }
    return sym->returnType ? sym->returnType : typeCtx.getError();
}

// The record type whose methods a call receiver exposes: unwraps `T?` for safe
// calls and maps a bounded type parameter to its bound class.
StructInfo* Analyzer::receiverStructInfo(const std::optional<ast::Expression>& obj,
                                         bool unwrapOptional) {
    if (!obj) return nullptr;
    Type* t = analysis.typeOf(obj->node.greenNode());
    if (!t) return nullptr;
    if (unwrapOptional) {
        if (!t->isOptional() || !t->inner) return nullptr;
        t = t->inner;
    }
    if (t->isTypeParam()) {
        return t->structInfo;  // primary bound (or null when unbounded)
    }
    return (t->hasRecordLayout() && t->structInfo) ? t->structInfo : nullptr;
}

Type* Analyzer::analyzeCall(const ast::CallExpression& expr) {
    Type* result = [&]() -> Type* {
    auto callee = expr.callee();
    auto args = expr.arguments();

    // Method call: obj.method(args)
    if (callee && callee->asMember()) {
        auto member = *callee->asMember();

        // Static builtin on the `string` type keyword: string.fromBytes(byte[]).
        if (auto objExpr = member.object()) {
            if (auto idObj = objExpr->asIdent()) {
                if (idObj->node.firstToken(SyntaxKind::KwString)) {
                    auto memberName = member.memberText();
                    if (memberName && *memberName == u"fromBytes") {
                        Type* byteArr = typeCtx.getArray(typeCtx.getPrimitive(TypeKind::Byte));
                        if (args.size() != 1) {
                            errorAtNode(expr.node, "'string.fromBytes' expects 1 argument (a byte[]), got " +
                                std::to_string(args.size()) + ".");
                            for (auto& a : args) analyzeExpr(a);
                        } else {
                            Type* argT = analyzeExpr(args[0]);
                            if (!argT->isError() && !byteArr->assignableFrom(argT)) {
                                errorAtNode(args[0].node, "'string.fromBytes' expects a byte[], got '" +
                                    argT->toString() + "'.");
                            }
                        }
                        return typeCtx.getPrimitive(TypeKind::String);
                    }
                    errorAtNode(expr.node, "'string' has no static member '" +
                        asciiOf(member.memberText().value_or(std::u16string{})) + "'.");
                    for (auto& a : args) analyzeExpr(a);
                    return typeCtx.getError();
                }
            }
        }

        // Namespace-qualified free-function call: ns.func(args). Resolve the function in
        // the imported module before treating the callee as a method (analyzeExpr below
        // would otherwise complain that the namespace has no such member).
        if (auto objExpr = member.object()) {
            if (auto idObj = objExpr->asIdent()) {
                if (auto idName = idObj->nameText()) {
                    Symbol* nsSym = currentScope ? currentScope->lookup(*idName) : nullptr;
                    auto memberName = member.memberText();
                    if (nsSym && nsSym->kind == SymbolKind::Namespace && memberName &&
                        nsSym->namespaceTarget) {
                        Symbol* fnSym = nsSym->namespaceTarget->globalSymbol(*memberName);
                        if (fnSym && fnSym->kind == SymbolKind::Function) {
                            analysis.setSymbol(idObj->node.greenNode(), nsSym);
                            const std::u16string& targetPackage =
                                nsSym->namespaceTarget->packagePrefix();
                            auto callableHere = [&](Symbol* s) {
                                return isTopLevelVisibleFrom(s->visibility,
                                    nsSym->namespaceModulePath, targetPackage);
                            };
                            std::vector<Symbol*> chain = overloadChainOf(fnSym);
                            if (chain.size() > 1 || callUsesNamedArguments(args)) {
                                analysis.setSymbol(member.node.greenNode(), fnSym);
                                CallShape shape = analyzeCallShape(args);
                                std::vector<OverloadCandidate> candidates;
                                for (Symbol* s : chain) {
                                    candidates.push_back({s, nullptr, callableHere(s)});
                                }
                                OverloadChoice choice = resolveOverloadedCall(
                                    candidates, shape, expr.node, asciiOf(*memberName), "Function");
                                if (choice.failed) return typeCtx.getError();
                                analysis.setSymbol(member.node.greenNode(), choice.symbol);
                                if (!choice.accessible) {
                                    errorAtNode(member.node, invisibleSymbolMessage("Function",
                                        *memberName, choice.symbol->visibility,
                                        nsSym->namespaceModulePath, targetPackage));
                                    return typeCtx.getError();
                                }
                                return checkResolvedCallArguments(shape, choice, expr.node.greenNode());
                            }
                            if (!callableHere(fnSym)) {
                                errorAtNode(member.node, invisibleSymbolMessage("Function",
                                    *memberName, fnSym->visibility,
                                    nsSym->namespaceModulePath, targetPackage));
                                for (auto& a : args) analyzeExpr(a);
                                return typeCtx.getError();
                            }
                            analysis.setSymbol(member.node.greenNode(), fnSym);
                            if (isFromCStringIntrinsic(fnSym)) {
                                return checkFromCStringCall(expr);
                            }
                            if (fnSym->isExternal) {
                                return analyzeExternalCall(expr, fnSym, *memberName);
                            }
                            if (fnSym->isTemplate) {
                                return analyzeGenericCall(expr, fnSym, *memberName);
                            }
                            return checkDirectCallArguments(expr, fnSym, *memberName);
                        }
                    }
                }
            }
        }

        // Built-in conversion methods on primitive and string receivers. These
        // have no Symbol; codegen recognizes them structurally. Intercept before
        // generic member resolution, which would reject a primitive receiver.
        if (auto objExpr = member.object()) {
            auto memberName = member.memberText();
            if (memberName && *memberName == u"toString") {
                Type* recvT = analyzeExpr(*objExpr);
                if (recvT->isError()) { for (auto& a : args) analyzeExpr(a); return typeCtx.getError(); }
                if (recvT->isInteger() || recvT->isBool() || recvT->isString() || recvT->isEnum()) {
                    if (!args.empty()) {
                        errorAtNode(expr.node, "'toString' takes no arguments.");
                        for (auto& a : args) analyzeExpr(a);
                    }
                    return typeCtx.getPrimitive(TypeKind::String);
                }
                if (recvT->isFloat() || recvT->kind == TypeKind::Decimal) {
                    errorAtNode(expr.node, "'.toString()' is not yet available for type '" +
                        recvT->toString() + "'.");
                    for (auto& a : args) analyzeExpr(a);
                    return typeCtx.getError();
                }
                // A struct without its own toString serializes to its JSON form; a struct
                // that declares one falls through to normal method resolution.
                if (recvT->isStruct() && recvT->structInfo &&
                    !declaredToString(recvT->structInfo)) {
                    if (!args.empty()) {
                        errorAtNode(expr.node, "'toString' takes no arguments.");
                        for (auto& a : args) analyzeExpr(a);
                    }
                    // A generic struct is judged per instantiation during code generation.
                    if (!TypeContext::containsTypeParam(recvT)) checkStructJsonable(recvT, expr.node);
                    return typeCtx.getPrimitive(TypeKind::String);
                }
                // Records may declare their own toString: fall through to resolution.
            }
            if (memberName && *memberName == u"toBytes") {
                Type* recvT = analyzeExpr(*objExpr);
                if (recvT->isError()) { for (auto& a : args) analyzeExpr(a); return typeCtx.getError(); }
                if (recvT->isString()) {
                    if (!args.empty()) {
                        errorAtNode(expr.node, "'toBytes' takes no arguments.");
                        for (auto& a : args) analyzeExpr(a);
                    }
                    return typeCtx.getArray(typeCtx.getPrimitive(TypeKind::Byte));
                }
                // Records may declare their own toBytes: fall through to resolution.
            }
            // String search builtins: indexOf(string) -> long, contains(string) -> bool.
            if (memberName && (*memberName == u"indexOf" || *memberName == u"contains")) {
                Type* recvT = analyzeExpr(*objExpr);
                if (recvT->isError()) { for (auto& a : args) analyzeExpr(a); return typeCtx.getError(); }
                if (recvT->isString()) {
                    bool isIndexOf = *memberName == u"indexOf";
                    std::string name = isIndexOf ? "indexOf" : "contains";
                    if (args.size() != 1) {
                        errorAtNode(expr.node, "'" + name + "' expects 1 argument (a string), got " +
                            std::to_string(args.size()) + ".");
                        for (auto& a : args) analyzeExpr(a);
                    } else {
                        Type* argT = analyzeExpr(args[0]);
                        if (!argT->isError() && !argT->isString()) {
                            errorAtNode(args[0].node, "'" + name + "' expects a string argument, got '" +
                                argT->toString() + "'.");
                        }
                    }
                    return typeCtx.getPrimitive(isIndexOf ? TypeKind::Long : TypeKind::Bool);
                }
                // Records may declare their own indexOf/contains: fall through to resolution.
            }
            // Range builtins: string.substring(start, end) -> string and
            // array slice(start, end) -> T[], both half-open ranges.
            auto checkRangeArguments = [&](const std::string& name) {
                if (args.size() != 2) {
                    errorAtNode(expr.node, "'" + name + "' expects 2 arguments (start and end), got " +
                        std::to_string(args.size()) + ".");
                    for (auto& a : args) analyzeExpr(a);
                    return;
                }
                for (auto& a : args) {
                    Type* argT = analyzeExpr(a);
                    if (!argT->isError() && !argT->isInteger()) {
                        errorAtNode(a.node, "'" + name + "' expects an integer index, got '" +
                            argT->toString() + "'.");
                    }
                }
            };
            if (memberName && *memberName == u"substring") {
                Type* recvT = analyzeExpr(*objExpr);
                if (recvT->isError()) { for (auto& a : args) analyzeExpr(a); return typeCtx.getError(); }
                if (recvT->isString()) {
                    checkRangeArguments("substring");
                    return typeCtx.getPrimitive(TypeKind::String);
                }
                // Records may declare their own substring: fall through to resolution.
            }
            if (memberName && *memberName == u"slice") {
                Type* recvT = analyzeExpr(*objExpr);
                if (recvT->isError()) { for (auto& a : args) analyzeExpr(a); return typeCtx.getError(); }
                if (recvT->isOptional() && recvT->inner && recvT->inner->isArray()) {
                    errorAtNode(expr.node, "Cannot call 'slice' on '" + recvT->toString() +
                        "' because it may be null. Check for null first.");
                    for (auto& a : args) analyzeExpr(a);
                    return typeCtx.getError();
                }
                if (recvT->isArray()) {
                    checkRangeArguments("slice");
                    return recvT;
                }
                // Records may declare their own slice: fall through to resolution.
            }
            // Every type is hashable: hash() resolves to a declared
            // `hash() -> long` when the receiver (or its bound) has one, and is
            // synthesized by codegen otherwise.
            if (memberName && *memberName == u"hash") {
                Type* recvT = analyzeExpr(*objExpr);
                if (recvT->isError()) { for (auto& a : args) analyzeExpr(a); return typeCtx.getError(); }
                if (recvT->isVoid()) {
                    errorAtNode(expr.node, "Cannot call 'hash' on a void expression.");
                    for (auto& a : args) analyzeExpr(a);
                    return typeCtx.getError();
                }
                bool declaresHash = false;
                if (recvT->isTypeParam()) {
                    for (StructInfo* b : boundsOfTypeParam(recvT)) {
                        if (b && b->classDeclaringMethod(u"hash")) { declaresHash = true; break; }
                    }
                } else if (recvT->hasRecordLayout() && recvT->structInfo) {
                    declaresHash = recvT->isClass()
                        ? recvT->structInfo->classDeclaringMethod(u"hash") != nullptr
                        : recvT->structInfo->findMethodIndex(u"hash") >= 0;
                }
                if (!declaresHash) {
                    if (!args.empty()) {
                        errorAtNode(expr.node, "'hash' takes no arguments.");
                        for (auto& a : args) analyzeExpr(a);
                    }
                    return typeCtx.getPrimitive(TypeKind::Long);
                }
                // The receiver declares its own hash(): fall through to resolution.
            }
        }

        analyzeExpr(*callee);  // resolves field-or-method on member
        auto* memberInfo = analysis.find(member.node.greenNode());
        Symbol* methodSym = memberInfo ? memberInfo->resolvedMethodSymbol : nullptr;
        if (methodSym) {
            auto mname = member.memberText().value_or(std::u16string{});
            StructInfo* recvInfo = receiverStructInfo(member.object(), /*unwrapOptional=*/false);
            std::vector<const MethodInfo*> cands = recvInfo
                ? collectMethodCandidates(recvInfo, mname) : std::vector<const MethodInfo*>{};
            if (cands.size() > 1 || callUsesNamedArguments(args)) {
                CallShape shape = analyzeCallShape(args);
                std::vector<OverloadCandidate> candidates;
                for (const MethodInfo* mi : cands) {
                    candidates.push_back({mi->symbol, mi,
                        isMemberAccessAllowed(mi->visibility, mi->definingClass)});
                }
                if (candidates.empty()) candidates.push_back({methodSym, nullptr, true});
                OverloadChoice choice = resolveOverloadedCall(
                    candidates, shape, expr.node, asciiOf(mname), "Method");
                if (choice.failed) return typeCtx.getError();
                if (!choice.accessible && choice.method) {
                    checkMemberAccess(member.node, mname, choice.method->visibility,
                                      choice.method->definingClass);
                }
                analysis.setMethodSymbol(member.node.greenNode(), choice.symbol);
                return checkResolvedCallArguments(shape, choice, expr.node.greenNode());
            }
            size_t req = requiredArgCount(methodSym);
            if (args.size() < req || args.size() > methodSym->paramTypes.size()) {
                errorAtNode(expr.node, "Method '" + asciiOf(mname) + "' expects " +
                    std::to_string(req) +
                    (req == methodSym->paramTypes.size() ? "" : "-" + std::to_string(methodSym->paramTypes.size())) +
                    " argument(s), got " + std::to_string(args.size()));
            }
            size_t n = std::min(args.size(), methodSym->paramTypes.size());
            for (size_t i = 0; i < n; ++i) {
                Type* paramT = methodSym->paramTypes[i];
                Type* argT = analyzeExprAdapt(args[i], paramT);
                if (!paramT->assignableFrom(argT)) {
                    errorAtNode(args[i].node, "Argument " + std::to_string(i + 1) +
                        ": expected '" + paramT->toString() + "', got '" + argT->toString() + "'");
                }
            }
            for (size_t i = n; i < args.size(); ++i) analyzeExpr(args[i]);
            return methodSym->returnType ? methodSym->returnType : typeCtx.getError();
        }
        for (auto& a : args) analyzeExpr(a);
        return typeCtx.getError();
    }

    // Safe method call: obj?.method(args)
    if (callee && callee->asSafeMember()) {
        auto member = *callee->asSafeMember();

        // A namespace is never null, so `ns?.func(args)` is meaningless.
        if (auto objExpr = member.object()) {
            if (auto idObj = objExpr->asIdent()) {
                if (auto idName = idObj->nameText()) {
                    Symbol* nsSym = currentScope ? currentScope->lookup(*idName) : nullptr;
                    if (nsSym && nsSym->kind == SymbolKind::Namespace) {
                        errorAtNode(expr.node, "'?.' cannot be used on the module namespace '" +
                            asciiOf(*idName) + "'; call it directly with '.'.");
                        for (auto& a : args) analyzeExpr(a);
                        return typeCtx.getError();
                    }
                }
            }
        }

        analyzeExpr(*callee);  // resolves field-or-method on safe-member
        auto* memberInfo = analysis.find(member.node.greenNode());
        Symbol* methodSym = memberInfo ? memberInfo->resolvedMethodSymbol : nullptr;
        if (methodSym) {
            auto mname = member.memberText().value_or(std::u16string{});
            StructInfo* recvInfo = receiverStructInfo(member.object(), /*unwrapOptional=*/true);
            std::vector<const MethodInfo*> cands = recvInfo
                ? collectMethodCandidates(recvInfo, mname) : std::vector<const MethodInfo*>{};
            if (cands.size() > 1 || callUsesNamedArguments(args)) {
                CallShape shape = analyzeCallShape(args);
                std::vector<OverloadCandidate> candidates;
                for (const MethodInfo* mi : cands) {
                    candidates.push_back({mi->symbol, mi,
                        isMemberAccessAllowed(mi->visibility, mi->definingClass)});
                }
                if (candidates.empty()) candidates.push_back({methodSym, nullptr, true});
                OverloadChoice choice = resolveOverloadedCall(
                    candidates, shape, expr.node, asciiOf(mname), "Method");
                if (choice.failed) return typeCtx.getError();
                if (!choice.accessible && choice.method) {
                    checkMemberAccess(member.node, mname, choice.method->visibility,
                                      choice.method->definingClass);
                }
                analysis.setMethodSymbol(member.node.greenNode(), choice.symbol);
                Type* ret = checkResolvedCallArguments(shape, choice, expr.node.greenNode());
                if (!ret || ret->isError()) return typeCtx.getError();
                if (ret->isVoid()) return ret;
                return typeCtx.getOptional(ret);
            }
            size_t req = requiredArgCount(methodSym);
            if (args.size() < req || args.size() > methodSym->paramTypes.size()) {
                errorAtNode(expr.node, "Method '" + asciiOf(mname) + "' expects " +
                    std::to_string(req) +
                    (req == methodSym->paramTypes.size() ? "" : "-" + std::to_string(methodSym->paramTypes.size())) +
                    " argument(s), got " + std::to_string(args.size()));
            }
            size_t n = std::min(args.size(), methodSym->paramTypes.size());
            for (size_t i = 0; i < n; ++i) {
                Type* paramT = methodSym->paramTypes[i];
                Type* argT = analyzeExprAdapt(args[i], paramT);
                if (!paramT->assignableFrom(argT)) {
                    errorAtNode(args[i].node, "Argument " + std::to_string(i + 1) +
                        ": expected '" + paramT->toString() + "', got '" + argT->toString() + "'");
                }
            }
            for (size_t i = n; i < args.size(); ++i) analyzeExpr(args[i]);
            Type* ret = methodSym->returnType;
            if (!ret || ret->isError()) return typeCtx.getError();
            // A void safe call is a statement: it runs only when the receiver
            // is non-null and produces no value.
            if (ret->isVoid()) return ret;
            return typeCtx.getOptional(ret);
        }
        for (auto& a : args) analyzeExpr(a);
        return typeCtx.getError();
    }

    // Base-constructor chaining: super(args)
    if (callee && callee->asSuper()) {
        analyzeExpr(*callee);
        sawSuperConstructorCall = true;
        StructInfo* cls = (currentThis && currentThis->type) ? currentThis->type->structInfo : nullptr;
        StructInfo* base = cls ? cls->baseInfo : nullptr;
        bool inCtor = cls && currentFunction && currentFunction->isConstructor;
        if (!inCtor) {
            errorAtNode(expr.node, "'super(...)' can only be called from a constructor");
        }
        if (!base) {
            for (auto& a : args) analyzeExpr(a);
            return typeCtx.getPrimitive(TypeKind::Void);
        }
        std::vector<const MethodInfo*> ctorCands;
        for (const auto& m : base->methods) {
            if (m.isConstructor && m.symbol) ctorCands.push_back(&m);
        }
        if (ctorCands.empty()) {
            if (!args.empty())
                errorAtNode(expr.node, "Base class '" + asciiOf(base->name) +
                    "' has no constructor, so 'super(...)' takes no arguments");
            for (auto& a : args) analyzeExpr(a);
            return typeCtx.getPrimitive(TypeKind::Void);
        }
        if (ctorCands.size() > 1 || callUsesNamedArguments(args)) {
            CallShape shape = analyzeCallShape(args);
            std::vector<OverloadCandidate> candidates;
            for (const MethodInfo* mi : ctorCands) {
                candidates.push_back({mi->symbol, mi,
                    isMemberAccessAllowed(mi->visibility,
                                          mi->definingClass ? mi->definingClass : base)});
            }
            OverloadChoice choice = resolveOverloadedCall(
                candidates, shape, expr.node, asciiOf(base->name), "Base constructor");
            if (choice.failed) return typeCtx.getPrimitive(TypeKind::Void);
            if (!choice.accessible && choice.method) {
                checkConstructorAccess(expr.node, base, *choice.method);
            }
            analysis.setMethodSymbol(callee->node.greenNode(), choice.symbol);
            checkResolvedCallArguments(shape, choice, expr.node.greenNode());
            return typeCtx.getPrimitive(TypeKind::Void);
        }
        checkConstructorAccess(expr.node, base, *ctorCands.front());
        Symbol* ctorSym = ctorCands.front()->symbol;
        analysis.setMethodSymbol(callee->node.greenNode(), ctorSym);
        size_t req = requiredArgCount(ctorSym);
        if (args.size() < req || args.size() > ctorSym->paramTypes.size()) {
            errorAtNode(expr.node, "Base constructor '" + asciiOf(base->name) + "' expects " +
                std::to_string(req) +
                (req == ctorSym->paramTypes.size() ? "" : "-" + std::to_string(ctorSym->paramTypes.size())) +
                " argument(s), got " + std::to_string(args.size()));
        }
        size_t n = std::min(args.size(), ctorSym->paramTypes.size());
        for (size_t i = 0; i < n; ++i) {
            Type* paramT = ctorSym->paramTypes[i];
            Type* argT = analyzeExprAdapt(args[i], paramT);
            if (!paramT->assignableFrom(argT)) {
                errorAtNode(args[i].node, "Argument " + std::to_string(i + 1) +
                    ": expected '" + paramT->toString() + "', got '" + argT->toString() + "'");
            }
        }
        for (size_t i = n; i < args.size(); ++i) analyzeExpr(args[i]);
        return typeCtx.getPrimitive(TypeKind::Void);
    }

    // Function call: name(args)
    auto idCallee = callee ? callee->asIdent() : std::nullopt;
    if (!idCallee) {
        errorAtNode(expr.node, "Only direct function calls are supported");
        for (auto& a : args) analyzeExpr(a);
        return typeCtx.getError();
    }
    auto name = idCallee->nameText();
    Symbol* sym = (name && currentScope) ? currentScope->lookup(*name) : nullptr;
    // `StructName(args)` builds a by-value struct through its constructor. A struct
    // name is not a scope symbol in its own module (only imported type names are),
    // so accept both the imported type-name symbol and a same-module lookup.
    if (name) {
        Type* structT = nullptr;
        if (sym && sym->isTypeName && sym->type && sym->type->isStruct()) {
            structT = sym->type;
        } else if (!sym) {
            Type* nt = typeCtx.lookupNamedType(modulePath_, *name);
            if (nt && nt->isStruct()) structT = nt;
        }
        if (structT) {
            // `Pair<C, C>(args)` instantiates the generic struct first, exactly like
            // 'new List<int>()', then resolves the constructor on the instantiation.
            auto typeArgs = expr.typeArguments();
            StructInfo* si = structT->structInfo;
            if (si && si->isTemplate && !typeArgs.empty()) {
                structT = instantiateFromArgs(structT, typeArgs, expr.node);
                if (structT->isError()) {
                    for (auto& a : args) analyzeExpr(a);
                    return structT;
                }
            } else if (!typeArgs.empty()) {
                errorAtNode(expr.node, "Struct '" + asciiOf(*name) +
                    "' is not generic and takes no type arguments.");
                for (auto& a : args) analyzeExpr(a);
                return typeCtx.getError();
            }
            return analyzeStructConstructorCall(expr, structT, *name);
        }
    }
    if (!sym) {
        errorAtNode(idCallee->node, "Undefined function '" +
            asciiOf(name.value_or(std::u16string{})) + "'");
        for (auto& a : args) analyzeExpr(a);
        return typeCtx.getError();
    }
    if (sym->kind != SymbolKind::Function) {
        errorAtNode(idCallee->node, "'" + asciiOf(*name) + "' is not a function");
        for (auto& a : args) analyzeExpr(a);
        return typeCtx.getError();
    }
    analysis.setSymbol(idCallee->node.greenNode(), sym);

    if (isFromCStringIntrinsic(sym)) {
        return checkFromCStringCall(expr);
    }
    if (sym->isExternal) {
        return analyzeExternalCall(expr, sym, *name);
    }
    if (sym->isTemplate) {
        return analyzeGenericCall(expr, sym, *name);
    }
    std::vector<Symbol*> chain = overloadChainOf(sym);
    if (chain.size() > 1 || callUsesNamedArguments(args)) {
        CallShape shape = analyzeCallShape(args);
        std::vector<OverloadCandidate> candidates;
        for (Symbol* s : chain) candidates.push_back({s, nullptr, true});
        OverloadChoice choice = resolveOverloadedCall(
            candidates, shape, expr.node, asciiOf(*name), "Function");
        if (choice.failed) return typeCtx.getError();
        analysis.setSymbol(idCallee->node.greenNode(), choice.symbol);
        return checkResolvedCallArguments(shape, choice, expr.node.greenNode());
    }
    return checkDirectCallArguments(expr, sym, *name);
    }();
    clearNarrowingsForCall(expr);
    return result;
}

Type* Analyzer::analyzeGenericCall(const ast::CallExpression& expr, Symbol* sym,
                                   const std::u16string& funcName) {
    auto args = expr.arguments();
    if (callUsesNamedArguments(args)) {
        errorAtNode(expr.node, "Generic function '" + asciiOf(funcName) +
            "' does not support named arguments; pass the arguments in order.");
        for (auto& a : args) {
            if (auto na = a.asNamedArgument()) {
                if (auto value = na->value()) analyzeExpr(*value);
            } else {
                analyzeExpr(a);
            }
        }
        return typeCtx.getError();
    }
    auto explicitArgs = expr.typeArguments();
    size_t arity = sym->typeParamNames.size();

    std::vector<Type*> typeArgs(arity, nullptr);
    if (!explicitArgs.empty()) {
        if (explicitArgs.size() != arity) {
            errorAtNode(expr.node, "Generic function '" + asciiOf(funcName) + "' expects " +
                std::to_string(arity) + (arity == 1 ? " type argument" : " type arguments") +
                ", but " + std::to_string(explicitArgs.size()) +
                (explicitArgs.size() == 1 ? " was given" : " were given"));
            for (auto& a : args) analyzeExpr(a);
            return typeCtx.getError();
        }
        for (size_t i = 0; i < arity; ++i) typeArgs[i] = resolveTypeReference(explicitArgs[i]);
    }

    std::vector<Type*> argTypes;
    argTypes.reserve(args.size());
    for (auto& a : args) argTypes.push_back(analyzeExpr(a));

    // Infer unspecified type args from arguments whose parameter is exactly a
    // type-parameter placeholder of this function (single-level inference).
    if (explicitArgs.empty()) {
        for (size_t i = 0; i < std::min(args.size(), sym->paramTypes.size()); ++i) {
            Type* pt = sym->paramTypes[i];
            if (pt && pt->isTypeParam() && pt->paramOwner == sym &&
                pt->paramIndex >= 0 && pt->paramIndex < static_cast<int>(arity) &&
                !typeArgs[pt->paramIndex] && argTypes[i] && !argTypes[i]->isError()) {
                typeArgs[pt->paramIndex] = argTypes[i];
            }
        }
    }

    for (size_t i = 0; i < arity; ++i) {
        if (!typeArgs[i]) {
            errorAtNode(expr.node, "Cannot infer type argument '" + asciiOf(sym->typeParamNames[i]) +
                "' for '" + asciiOf(funcName) + "'; pass it explicitly, e.g. '" +
                asciiOf(funcName) + "<int>(...)'");
            return typeCtx.getError();
        }
    }

    bool ok = true;
    static const std::vector<StructInfo*> kNoBounds;
    for (size_t i = 0; i < arity; ++i) {
        const std::vector<StructInfo*>& bounds =
            i < sym->typeParamBounds.size() ? sym->typeParamBounds[i] : kNoBounds;
        SyntaxNode diag = (i < explicitArgs.size()) ? explicitArgs[i].node : expr.node;
        if (!checkTypeArgBound(typeArgs[i], bounds, sym->typeParamNames[i], diag)) ok = false;
    }

    size_t req = requiredArgCount(sym);
    if (args.size() < req || args.size() > sym->paramTypes.size()) {
        errorAtNode(expr.node, "Function '" + asciiOf(funcName) + "' expects " +
            std::to_string(req) +
            (req == sym->paramTypes.size() ? "" : "-" + std::to_string(sym->paramTypes.size())) +
            " argument(s), got " + std::to_string(args.size()));
        ok = false;
    }
    size_t n = std::min(args.size(), sym->paramTypes.size());
    for (size_t i = 0; i < n; ++i) {
        Type* paramT = typeCtx.substitute(sym->paramTypes[i], sym, typeArgs);
        tryAdaptIntegerLiteral(args[i], paramT);
        Type* argT = argTypes[i];
        if (paramT && argT && !paramT->isError() && !argT->isError() &&
            !paramT->assignableFrom(argT)) {
            errorAtNode(args[i].node, "Argument " + std::to_string(i + 1) +
                ": expected '" + paramT->toString() + "', got '" + argT->toString() + "'");
        }
    }

    if (ok) {
        analysis.setCallTypeArgs(expr.node.greenNode(), typeArgs);
        typeCtx.recordFunctionInstantiation(sym, typeArgs);
    }
    Type* ret = typeCtx.substitute(sym->returnType, sym, typeArgs);
    return ret ? ret : typeCtx.getError();
}

// Argument count/type checking shared by plain `name(args)` and namespace-qualified
// `ns.name(args)` free-function calls. Returns the call's result type.
bool Analyzer::isFromCStringIntrinsic(const Symbol* sym) {
    return sym && sym->kind == SymbolKind::Function && sym->name == u"fromCString" &&
           sym->modulePath == u"std.ffi";
}

// std.ffi.fromCString reads a foreign NUL-terminated buffer into a fresh string, or null when the
// handle is null. Its type parameter accepts any external type (there is no common supertype to
// write in the signature), optional or not, so the argument check is intrinsic and the call is
// never instantiated as a generic.
Type* Analyzer::checkFromCStringCall(const ast::CallExpression& expr) {
    auto args = expr.arguments();
    Type* result = typeCtx.getOptional(typeCtx.getPrimitive(TypeKind::String));
    if (args.size() != 1) {
        errorAtNode(expr.node, "'fromCString' expects exactly 1 argument (a foreign handle), got " +
            std::to_string(args.size()) + ".");
        for (auto& a : args) { if (!a.asOutArgument()) analyzeExpr(a); }
        return result;
    }
    if (args[0].asOutArgument()) {
        errorAtNode(args[0].node, "'out' can only be used when calling an external function.");
        return result;
    }
    Type* argT = analyzeExpr(args[0]);
    Type* base = (argT && argT->isOptional()) ? argT->inner : argT;
    if (argT && !argT->isError() && !(base && base->isExternal())) {
        errorAtNode(args[0].node, "'fromCString' takes a value of an external type (a foreign "
            "handle), got '" + argT->toString() + "'. Pass a handle returned by a C function.");
    }
    return result;
}

Type* Analyzer::checkDirectCallArguments(const ast::CallExpression& expr, Symbol* sym,
                                         const std::u16string& funcName) {
    auto args = expr.arguments();
    size_t req = requiredArgCount(sym);
    if (args.size() < req || args.size() > sym->paramTypes.size()) {
        errorAtNode(expr.node, "Function '" + asciiOf(funcName) + "' expects " +
            std::to_string(req) +
            (req == sym->paramTypes.size() ? "" : "-" + std::to_string(sym->paramTypes.size())) +
            " argument(s), got " + std::to_string(args.size()));
    }
    size_t n = std::min(args.size(), sym->paramTypes.size());
    for (size_t i = 0; i < n; ++i) {
        if (args[i].asOutArgument()) {
            errorAtNode(args[i].node, "'out' can only be used when calling an external function.");
            continue;
        }
        Type* paramT = sym->paramTypes[i];
        Type* argT = analyzeExprAdapt(args[i], paramT);
        if (!paramT->assignableFrom(argT)) {
            errorAtNode(args[i].node, "Argument " + std::to_string(i + 1) +
                ": expected '" + paramT->toString() + "', got '" + argT->toString() + "'");
        }
    }
    for (size_t i = n; i < args.size(); ++i) {
        if (args[i].asOutArgument()) {
            errorAtNode(args[i].node, "'out' can only be used when calling an external function.");
        } else {
            analyzeExpr(args[i]);
        }
    }
    return sym->returnType ? sym->returnType : typeCtx.getError();
}

Type* Analyzer::analyzeExternalCall(const ast::CallExpression& expr, Symbol* sym,
                                    const std::u16string& funcName) {
    auto args = expr.arguments();
    if (callUsesNamedArguments(args)) {
        errorAtNode(expr.node, "External function '" + asciiOf(funcName) +
            "' does not support named arguments; pass the arguments in order.");
        for (auto& a : args) {
            if (auto na = a.asNamedArgument()) {
                if (auto value = na->value()) analyzeExpr(*value);
            } else if (!a.asOutArgument()) {
                analyzeExpr(a);
            }
        }
        return sym->returnType ? sym->returnType : typeCtx.getError();
    }
    size_t expected = sym->paramTypes.size();
    if (args.size() != expected) {
        errorAtNode(expr.node, "External function '" + asciiOf(funcName) + "' expects " +
            std::to_string(expected) + " argument(s), got " + std::to_string(args.size()));
    }
    size_t n = std::min(args.size(), expected);
    for (size_t i = 0; i < n; ++i) {
        Type* paramT = sym->paramTypes[i];
        bool wantsOut = sym->paramIsOut[i];
        auto& arg = args[i];
        auto outArg = arg.asOutArgument();
        if (wantsOut) {
            if (!outArg) {
                errorAtNode(arg.node, "Argument " + std::to_string(i + 1) +
                    " of '" + asciiOf(funcName) + "' is an 'out' parameter; pass a local variable as 'out <name>'.");
                continue;
            }
            auto identName = outArg->nameText();
            if (!identName) continue;
            Symbol* local = currentScope ? currentScope->lookup(*identName) : nullptr;
            if (!local || (local->kind != SymbolKind::Variable && local->kind != SymbolKind::Parameter)) {
                errorAtNode(arg.node, "'out " + asciiOf(*identName) +
                    "' must refer to a local variable or parameter.");
                continue;
            }
            if (local->isConst) {
                errorAtNode(arg.node, "Cannot pass '" + asciiOf(*identName) +
                    "' as 'out' because it is declared as constant");
            }
            if (auto identTok = outArg->identifier()) {
                analysis.setSymbol(identTok->greenNode(), local);
            }
            if (!local->type || !paramT->equals(local->type)) {
                std::string localType = local->type ? local->type->toString() : std::string("<unknown>");
                errorAtNode(arg.node, "Argument " + std::to_string(i + 1) +
                    " of '" + asciiOf(funcName) + "' expects 'out " + paramT->toString() +
                    "', got 'out " + localType + "'.");
                continue;
            }
            local->reassigned = true;
            if (currentScope) {
                currentScope->clearNarrowingsForRoot(local);
                currentScope->clearNarrowingsForIndexSymbol(local);
            }
        } else {
            if (outArg) {
                errorAtNode(arg.node, "Argument " + std::to_string(i + 1) +
                    " of '" + asciiOf(funcName) + "' is not an 'out' parameter; remove 'out'.");
                continue;
            }
            Type* argT = analyzeExprAdapt(arg, paramT);
            if (!paramT->assignableFrom(argT)) {
                errorAtNode(arg.node, "Argument " + std::to_string(i + 1) +
                    ": expected '" + paramT->toString() + "', got '" + argT->toString() + "'");
            }
        }
    }
    for (size_t i = n; i < args.size(); ++i) {
        if (auto outArg = args[i].asOutArgument()) {
            if (auto identName = outArg->nameText()) {
                Symbol* local = currentScope ? currentScope->lookup(*identName) : nullptr;
                if (local) {
                    if (auto identTok = outArg->identifier()) {
                        analysis.setSymbol(identTok->greenNode(), local);
                    }
                }
            }
        } else {
            analyzeExpr(args[i]);
        }
    }
    return sym->returnType ? sym->returnType : typeCtx.getError();
}

Type* Analyzer::analyzeMember(const ast::MemberExpression& expr) {
    auto obj = expr.object();
    if (!obj) return typeCtx.getError();

    // Namespace alias on the LHS: `ns.Name`, resolve `Name` against the
    // imported module's exported symbols rather than complaining about a
    // non-record type.
    if (auto idObj = obj->asIdent()) {
        if (auto idName = idObj->nameText()) {
            Symbol* nsSym = currentScope ? currentScope->lookup(*idName) : nullptr;
            if (nsSym && nsSym->kind == SymbolKind::Namespace) {
                analysis.setSymbol(idObj->node.greenNode(), nsSym);
                auto memberName = expr.memberText();
                if (!memberName) return typeCtx.getError();
                if (Type* t = typeCtx.lookupNamedType(nsSym->namespaceModulePath, *memberName)) {
                    if (!isTypeVisibleFrom(t)) {
                        errorAtNode(expr.node, invisibleTypeMessage(*memberName, t));
                        return typeCtx.getError();
                    }
                    analysis.setType(expr.node.greenNode(), t);
                    return t;
                }
                errorAtNode(expr.node, "Module '" + asciiOf(nsSym->namespaceModulePath) +
                    "' has no '" + asciiOf(*memberName) + "'");
                return typeCtx.getError();
            }
        }
    }

    // Enum member access. The object must denote an enum type (a
    // bare in-module enum name or an imported enum type alias), not a value of
    // enum type.
    if (auto idObj = obj->asIdent()) {
        if (auto idName = idObj->nameText()) {
            Symbol* valSym = currentScope ? currentScope->lookup(*idName) : nullptr;
            Type* enumType = nullptr;
            if (!valSym) {
                enumType = typeCtx.lookupEnum(modulePath_, *idName);
            } else if (valSym->isTypeName && valSym->type && valSym->type->isEnum()) {
                enumType = valSym->type;
                analysis.setSymbol(idObj->node.greenNode(), valSym);
            }
            if (enumType && enumType->structInfo) {
                analysis.setType(idObj->node.greenNode(), enumType);
                auto memberName = expr.memberText();
                if (!memberName) return typeCtx.getError();
                for (auto& m : enumType->structInfo->enumMembers) {
                    if (m.name == *memberName) {
                        analysis.setEnumConstant(expr.node.greenNode(), m.value);
                        analysis.setType(expr.node.greenNode(), enumType);
                        return enumType;
                    }
                }
                errorAtNode(expr.node, "'" + asciiOf(*memberName) + "' is not a member of enum '" +
                    enumType->toString() + "'");
                return typeCtx.getError();
            }
        }
    }

    Type* objT = analyzeExpr(*obj);
    if (objT->isError()) return typeCtx.getError();
    // A bounded type parameter exposes the members of every one of its bounds;
    // the first bound declaring the member wins.
    if (objT->isTypeParam()) {
        auto wanted = expr.memberText();
        Type* rebound = nullptr;
        for (StructInfo* b : boundsOfTypeParam(objT)) {
            if (!b) continue;
            Type* bt = b->templateOf ? typeCtx.typeForInstance(b)
                                     : typeCtx.lookupNamedType(b->modulePath, b->name);
            if (!bt || !bt->structInfo) continue;
            if (!rebound) rebound = bt;
            if (wanted && (bt->structInfo->findFieldIndex(*wanted) >= 0 ||
                           bt->structInfo->classDeclaringMethod(*wanted))) {
                rebound = bt;
                break;
            }
        }
        if (rebound) objT = rebound;
    }
    if (objT->isArray()) {
        auto memberName = expr.memberText();
        if (!memberName) return typeCtx.getError();
        if (*memberName == u"length") {
            return typeCtx.getPrimitive(TypeKind::Long);
        }
        errorAtNode(expr.node, "Type '" + objT->toString() + "' has no member '" +
            asciiOf(*memberName) + "'; arrays only have 'length'.");
        return typeCtx.getError();
    }
    if (objT->isString()) {
        auto memberName = expr.memberText();
        if (!memberName) return typeCtx.getError();
        if (*memberName == u"length") {
            return typeCtx.getPrimitive(TypeKind::Long);
        }
        errorAtNode(expr.node, "Type 'string' has no member '" +
            asciiOf(*memberName) + "'.");
        return typeCtx.getError();
    }
    if (!objT->hasRecordLayout() || !objT->structInfo) {
        if (objT->isOptional()) {
            errorAtNode(expr.node, "Cannot read a member of '" + objT->toString() +
                "' because it may be null. Use '?.' or check for null first.");
        } else {
            errorAtNode(expr.node, "Cannot read a member of '" + objT->toString() +
                "' because it has no members.");
        }
        return typeCtx.getError();
    }
    auto memberName = expr.memberText();
    if (!memberName) return typeCtx.getError();
    int idx = objT->structInfo->findFieldIndex(*memberName);
    if (idx >= 0) {
        const FieldInfo& fld = objT->structInfo->fields[idx];
        checkMemberAccess(expr.node, *memberName, fld.visibility, fld.definingClass);
        Type* fieldT = fld.type;
        // A nullable field can be null-narrowed; a class field can be
        // is-narrowed to a subclass.
        if (fieldT && (fieldT->isOptional() || fieldT->isClass()) && currentScope) {
            ast::Expression whole{expr.node};
            if (auto path = buildNarrowingPath(whole)) {
                if (Type* narrowed = currentScope->lookupNarrowedType(*path)) {
                    return narrowed;
                }
            }
        }
        return fieldT;
    }
    if (StructInfo* decl = objT->structInfo->classDeclaringMethod(*memberName)) {
        const MethodInfo& mi = decl->methods[decl->findMethodIndex(*memberName)];
        // An overloaded name defers visibility checking to call resolution,
        // where the chosen overload is known.
        if (collectMethodCandidates(objT->structInfo, *memberName).size() <= 1) {
            checkMemberAccess(expr.node, *memberName, mi.visibility, mi.definingClass);
        }
        analysis.setMethodSymbol(expr.node.greenNode(), mi.symbol);
        return typeCtx.getError();  // callee reference - not a value
    }
    errorAtNode(expr.node, "No field or method '" + asciiOf(*memberName) +
        "' on type '" + objT->toString() + "'");
    return typeCtx.getError();
}

Type* Analyzer::analyzeSafeMember(const ast::SafeMemberExpression& expr) {
    auto obj = expr.object();
    if (!obj) return typeCtx.getError();
    Type* objT = analyzeExpr(*obj);
    if (objT->isError()) return typeCtx.getError();
    objT = presenceOperandType(*obj, objT);

    if (!objT->isOptional()) {
        errorAtNode(expr.node, "The value on the left of '?.' has type '" + objT->toString() +
            "', which can never be null. Use '.' to access its members.");
        return typeCtx.getError();
    }
    Type* inner = objT->inner;
    auto memberName = expr.memberText();
    if (!memberName) return typeCtx.getError();
    if (inner && (inner->isString() || inner->isArray()) && *memberName == u"length") {
        return typeCtx.getOptional(typeCtx.getPrimitive(TypeKind::Long));
    }
    if (!inner || !inner->hasRecordLayout() || !inner->structInfo) {
        std::string innerName = inner ? inner->toString() : std::string("?");
        errorAtNode(expr.node, "The value on the left of '?.' has type '" + innerName +
            "?', which has no members to access.");
        return typeCtx.getError();
    }

    int idx = inner->structInfo->findFieldIndex(*memberName);
    if (idx >= 0) {
        const FieldInfo& fld = inner->structInfo->fields[idx];
        checkMemberAccess(expr.node, *memberName, fld.visibility, fld.definingClass);
        return typeCtx.getOptional(fld.type);
    }
    if (StructInfo* decl = inner->structInfo->classDeclaringMethod(*memberName)) {
        const MethodInfo& mi = decl->methods[decl->findMethodIndex(*memberName)];
        if (collectMethodCandidates(inner->structInfo, *memberName).size() <= 1) {
            checkMemberAccess(expr.node, *memberName, mi.visibility, mi.definingClass);
        }
        analysis.setMethodSymbol(expr.node.greenNode(), mi.symbol);
        return typeCtx.getError();
    }
    errorAtNode(expr.node, "No field or method named '" + asciiOf(*memberName) +
        "' on '" + inner->toString() + "'.");
    return typeCtx.getError();
}

// Guidance appended when a plain enum is used in a numeric conversion: it names
// the enum and shows how to make it numeric using its own first member.
static std::string enumNeedsValuesMessage(Type* enumT) {
    std::string name = enumT->toString();
    std::string member = "First";
    if (enumT->structInfo && !enumT->structInfo->enumMembers.empty())
        member = asciiOf(enumT->structInfo->enumMembers.front().name);
    return "Enum '" + name + "' has no assigned values; give its members explicit values (for "
        "example '" + member + " = 0') to convert it to and from integers.";
}

Type* Analyzer::analyzeCast(const ast::CastExpression& expr) {
    auto src = expr.source();
    auto tr = expr.targetType();
    if (!src || !tr) return typeCtx.getError();
    Type* srcT = analyzeExpr(*src);
    Type* dstT = resolveTypeReference(*tr);
    if (srcT->isError() || dstT->isError()) return typeCtx.getError();

    // A numeric enum yields its member's assigned value; the target must be an
    // integer type (widening to 'long' or narrowing per the usual 'as' rules).
    if (srcT->isEnum()) {
        if (!srcT->structInfo || !srcT->structInfo->enumIsNumeric) {
            errorAtNode(expr.node, enumNeedsValuesMessage(srcT));
            return typeCtx.getError();
        }
        if (!dstT->isInteger()) {
            errorAtNode(expr.node, "Cannot convert enum '" + srcT->toString() + "' to '" +
                dstT->toString() + "' with 'as'; a numeric enum converts to an integer type such "
                "as 'int' or 'long'.");
            return typeCtx.getError();
        }
        return dstT;
    }

    // An integer to enum conversion may find no matching member, so it is offered
    // only as the checked 'as?'; plain 'as' would be unsound.
    if (dstT->isEnum()) {
        if (dstT->structInfo && dstT->structInfo->enumIsNumeric) {
            errorAtNode(expr.node, "Converting '" + srcT->toString() + "' to enum '" +
                dstT->toString() + "' can fail when no member has that value; use 'as?', which "
                "yields '" + dstT->toString() + "?' (null when no member matches).");
        } else {
            errorAtNode(expr.node, enumNeedsValuesMessage(dstT));
        }
        return typeCtx.getError();
    }

    auto isNumeric = [](Type* t) { return t && (t->isInteger() || t->isFloat()); };
    if (!isNumeric(srcT) || !isNumeric(dstT)) {
        errorAtNode(expr.node, "Cannot cast '" + srcT->toString() + "' to '" +
            dstT->toString() + "'; 'as' only supports numeric conversions.");
        return typeCtx.getError();
    }
    return dstT;
}

bool Analyzer::checkClassTypeTest(Type* srcT, Type* dstT, bool isCast,
                                  const SyntaxNode& diagNode) {
    std::string op = isCast ? "as?" : "is";
    if (dstT->isOptional()) {
        std::string inner = dstT->inner ? dstT->inner->toString() : std::string("?");
        if (isCast) {
            errorAtNode(diagNode, "The target of 'as?' cannot be nullable; 'as? " + inner +
                "' already produces '" + inner + "?'. Drop the '?' on the target.");
        } else {
            errorAtNode(diagNode, "The target of 'is' cannot be nullable; 'is' is never true "
                "for null. Test against '" + inner + "' instead.");
        }
        return false;
    }
    if (dstT->isTypeParam()) {
        errorAtNode(diagNode, "The target of '" + op + "' must be a concrete class; '" +
            dstT->toString() + "' is a type parameter.");
        return false;
    }
    if (!dstT->isClass() || !dstT->structInfo) {
        errorAtNode(diagNode, "The target of '" + op + "' must be a class or an interface, got '" +
            dstT->toString() + "'.");
        return false;
    }
    Type* srcCore = srcT->isOptional() ? srcT->inner : srcT;
    if (!srcCore) return false;
    // A scrutinee mentioning a type parameter is checked per instantiation
    // during code generation, like interpolation holes.
    if (TypeContext::containsTypeParam(srcT) || TypeContext::containsTypeParam(dstT)) {
        return true;
    }
    if (!srcCore->isClass() || !srcCore->structInfo) {
        errorAtNode(diagNode, "Cannot use '" + op + "' on a value of type '" + srcT->toString() +
            "'; only class values can be " + (isCast ? "cast." : "tested."));
        return false;
    }
    if (srcCore->structInfo->isSubclassOrConforms(dstT->structInfo)) {
        // A nullable scrutinee still gains information from 'is': the test
        // also proves the value is not null.
        if (!isCast && srcT->isOptional()) return true;
        if (isCast) {
            errorAtNode(diagNode, "'as? " + dstT->toString() + "' is not needed here: a value "
                "of type '" + srcT->toString() + "' always converts to '" + dstT->toString() +
                "?'. Use the value directly.");
        } else {
            errorAtNode(diagNode, "A value of type '" + srcT->toString() + "' is always a '" +
                dstT->toString() + "'; this 'is' test would always be true. Remove it.");
        }
        return false;
    }
    // An interface scrutinee leaves everything to the runtime: any class or
    // interface target may match depending on the value's dynamic type.
    if (srcCore->isInterface()) return true;
    if (dstT->isInterface()) {
        // Class scrutinee against an interface target: a subclass may implement
        // it, unless the class is final (no subclasses can exist).
        if (srcCore->structInfo->isFinal) {
            errorAtNode(diagNode, "A value of type '" + srcT->toString() + "' can never be a '" +
                dstT->toString() + "': '" + srcCore->toString() + "' is 'final' and does not "
                "implement it. " + (isCast ? "This 'as?' cast would always be null."
                                           : "This 'is' test would always be false.") +
                " Remove it.");
            return false;
        }
        return true;
    }
    if (!dstT->structInfo->isSubclassOf(srcCore->structInfo)) {
        errorAtNode(diagNode, "A value of type '" + srcT->toString() + "' can never be a '" +
            dstT->toString() + "'; " + (isCast ? "this 'as?' cast would always be null."
                                               : "this 'is' test would always be false.") +
            " Remove it.");
        return false;
    }
    return true;
}

StructInfo* Analyzer::checkTypeArmTarget(Type* scrutType, Type* inner, Type* armT,
                                         const SyntaxNode& diagNode) {
    if (armT->isOptional()) {
        std::string name = armT->inner ? armT->inner->toString() : std::string("?");
        errorAtNode(diagNode, "The type of an 'is' arm cannot be nullable; 'is' never matches "
            "null. Test against '" + name + "' and handle null with a 'null ->' arm.");
        return nullptr;
    }
    if (armT->isTypeParam()) {
        errorAtNode(diagNode, "The type of an 'is' arm must be a concrete class; '" +
            armT->toString() + "' is a type parameter.");
        return nullptr;
    }
    if (!armT->isClass() || !armT->structInfo) {
        errorAtNode(diagNode, "The type of an 'is' arm must be a class or an interface, got '" +
            armT->toString() + "'.");
        return nullptr;
    }
    if (armT->structInfo == inner->structInfo) {
        errorAtNode(diagNode, "'is " + armT->toString() + "' matches every value of this switch "
            "over '" + scrutType->toString() + "'; use a 'default' arm instead.");
        return nullptr;
    }
    if (inner->structInfo->isSubclassOrConforms(armT->structInfo)) {
        errorAtNode(diagNode, "A value of type '" + inner->toString() + "' is always a '" +
            armT->toString() + "'; this arm would match every value. Use a 'default' arm instead.");
        return nullptr;
    }
    // An interface scrutinee (or an interface arm over a non-final class) is
    // decided at runtime; only the impossible combinations are rejected.
    if (inner->isInterface()) return armT->structInfo;
    if (armT->isInterface()) {
        if (inner->structInfo->isFinal) {
            errorAtNode(diagNode, "A value of type '" + inner->toString() + "' can never be a '" +
                armT->toString() + "': '" + inner->toString() + "' is 'final' and does not "
                "implement it. This arm would never match. Remove it.");
            return nullptr;
        }
        return armT->structInfo;
    }
    if (!armT->structInfo->isSubclassOf(inner->structInfo)) {
        errorAtNode(diagNode, "A value of type '" + inner->toString() + "' can never be a '" +
            armT->toString() + "'; this arm would never match. Remove it.");
        return nullptr;
    }
    return armT->structInfo;
}

Type* Analyzer::analyzeTypeTest(const ast::TypeTestExpression& expr) {
    auto operand = expr.operand();
    auto tr = expr.targetType();
    if (!operand || !tr) return typeCtx.getError();
    Type* srcT = analyzeExpr(*operand);
    Type* dstT = resolveTypeReference(*tr);
    if (srcT->isError() || dstT->isError()) return typeCtx.getError();
    if (!checkClassTypeTest(srcT, dstT, /*isCast=*/false, expr.node)) return typeCtx.getError();
    return typeCtx.getPrimitive(TypeKind::Bool);
}

Type* Analyzer::analyzeCheckedCast(const ast::CheckedCastExpression& expr) {
    auto src = expr.source();
    auto tr = expr.targetType();
    if (!src || !tr) return typeCtx.getError();
    Type* srcT = analyzeExpr(*src);
    Type* dstT = resolveTypeReference(*tr);
    if (srcT->isError() || dstT->isError()) return typeCtx.getError();

    // An integer to numeric-enum conversion: the result is the matching member,
    // or null when no member has that value.
    if (dstT->isEnum()) {
        if (!dstT->structInfo || !dstT->structInfo->enumIsNumeric) {
            errorAtNode(expr.node, enumNeedsValuesMessage(dstT));
            return typeCtx.getError();
        }
        if (!srcT->isInteger()) {
            errorAtNode(expr.node, "Cannot convert '" + srcT->toString() + "' to enum '" +
                dstT->toString() + "' with 'as?'; the value being matched must be an integer.");
            return typeCtx.getError();
        }
        return typeCtx.getOptional(dstT);
    }

    // enum -> integer never fails, so 'as?' is the wrong tool; point at 'as'.
    if (srcT->isEnum() && srcT->structInfo && srcT->structInfo->enumIsNumeric && dstT->isInteger()) {
        errorAtNode(expr.node, "Converting enum '" + srcT->toString() + "' to '" +
            dstT->toString() + "' always succeeds; use 'as', not 'as?'.");
        return typeCtx.getError();
    }

    if (!checkClassTypeTest(srcT, dstT, /*isCast=*/true, expr.node)) return typeCtx.getError();
    return typeCtx.getOptional(dstT);
}

Type* Analyzer::analyzeSubscript(const ast::SubscriptExpression& expr) {
    auto obj = expr.object();
    auto idx = expr.index();
    if (!obj || !idx) return typeCtx.getError();
    Type* objT = analyzeExpr(*obj);
    Type* idxT = analyzeExpr(*idx);
    if (objT->isError()) return typeCtx.getError();
    if (objT->isOptional() && objT->inner && objT->inner->isArray()) {
        errorAtNode(expr.node, "Cannot index '" + objT->toString() +
            "' because it may be null. Check for null first.");
        return typeCtx.getError();
    }
    if (!objT->isArray()) {
        errorAtNode(expr.node, "Cannot index '" + objT->toString() +
            "'; only arrays support [] indexing.");
        return typeCtx.getError();
    }
    if (!idxT->isError() && !idxT->isInteger()) {
        errorAtNode(idx->node, "Array index must be an integer, got '" + idxT->toString() + "'");
    }
    Type* elemT = objT->inner ? objT->inner : typeCtx.getError();
    if (elemT && (elemT->isOptional() || elemT->isClass()) && currentScope) {
        ast::Expression whole{expr.node};
        if (auto path = buildNarrowingPath(whole)) {
            if (Type* narrowed = currentScope->lookupNarrowedType(*path)) {
                return narrowed;
            }
        }
    }
    return elemT;
}

Type* Analyzer::analyzeSafeSubscript(const ast::SafeSubscriptExpression& expr) {
    auto obj = expr.object();
    auto idx = expr.index();
    if (!obj || !idx) return typeCtx.getError();
    Type* objT = analyzeExpr(*obj);
    Type* idxT = analyzeExpr(*idx);
    if (objT->isError()) return typeCtx.getError();
    objT = presenceOperandType(*obj, objT);

    if (!objT->isOptional()) {
        errorAtNode(expr.node, "The value on the left of '?[' has type '" + objT->toString() +
            "', which can never be null. Use '[' to index it.");
        return typeCtx.getError();
    }
    Type* inner = objT->inner;
    if (!inner || !inner->isArray()) {
        std::string innerName = inner ? inner->toString() : std::string("?");
        errorAtNode(expr.node, "The value on the left of '?[' has type '" + objT->toString() +
            "', which is not a nullable array.");
        return typeCtx.getError();
    }
    Type* elem = inner->inner;
    if (!elem) return typeCtx.getError();
    if (!idxT->isError() && !idxT->isInteger()) {
        errorAtNode(idx->node, "Array index must be an integer, got '" + idxT->toString() + "'");
    }
    return typeCtx.getOptional(elem);
}

void Analyzer::invalidateNarrowingsForWrite(const ast::Expression& target) {
    if (!currentScope) return;
    if (auto id = target.asIdent()) {
        if (auto* info = analysis.find(id->node.greenNode())) {
            if (Symbol* sym = info->resolvedSymbol) {
                currentScope->clearNarrowingsForRoot(sym);
                currentScope->clearNarrowingsForIndexSymbol(sym);
                return;
            }
        }
    }
    if (target.asMember() || target.asSubscript()) {
        if (auto p = buildNarrowingPath(target, nullptr, /*allowAnyIndex=*/true)) {
            currentScope->clearNarrowingsThatMayAlias(*p);
        }
    }
}

Type* Analyzer::declaredBindingType(const ast::Expression& operand) const {
    ast::Expression core = unwrapParens(operand);
    if (auto id = core.asIdent()) {
        if (auto* info = analysis.find(id->node.greenNode())) {
            if (Symbol* sym = info->resolvedSymbol) return sym->type;
        }
    } else if (core.asThis()) {
        if (currentThis) return currentThis->type;
    } else if (auto m = core.asMember()) {
        auto obj = m->object();
        auto name = m->memberText();
        Type* objT = obj ? analysis.typeOf(obj->node.greenNode()) : nullptr;
        if (name && objT && objT->structInfo) {
            int idx = objT->structInfo->findFieldIndex(*name);
            if (idx >= 0) return objT->structInfo->fields[idx].type;
        }
    } else if (auto su = core.asSubscript()) {
        auto obj = su->object();
        Type* objT = obj ? analysis.typeOf(obj->node.greenNode()) : nullptr;
        if (objT && objT->isArray()) return objT->inner;
    }
    return nullptr;
}

Type* Analyzer::presenceOperandType(const ast::Expression& operand, Type* analyzed) {
    Type* declared = declaredBindingType(operand);
    if (!declared || !declared->isOptional() || declared->equals(analyzed)) return analyzed;
    analysis.setType(operand.node.greenNode(), declared);
    ast::Expression core = unwrapParens(operand);
    analysis.setType(core.node.greenNode(), declared);
    return declared;
}

void Analyzer::establishAssignmentNarrowing(Symbol* sym, Type* valueT) {
    if (!sym || !currentScope || !valueT) return;
    if (sym->kind != SymbolKind::Variable && sym->kind != SymbolKind::Parameter) return;
    Type* declared = sym->type;
    if (!declared || !declared->isOptional() || !declared->inner) return;
    if (valueT->isError() || valueT->isNull() || valueT->isOptional()) return;
    if (!declared->inner->assignableFrom(valueT)) return;
    currentScope->narrowedTypes[NarrowingPath{sym, {}}] = declared->inner;
}

Type* Analyzer::analyzeAssign(const ast::AssignExpression& expr) {
    auto target = expr.target();
    auto value = expr.value();
    if (!target || !value) return typeCtx.getError();
    // A plain `x = ...` is a definite assignment of x, so its own identifier on the
    // left is a write, not a read to check. A compound `x += ...` reads x first, and
    // a member or subscript target (`x.f = ...`, `x[i] = ...`) reads its base, which
    // must already be assigned, so only a simple-assignment plain identifier is exempt.
    auto opTok = expr.operatorToken();
    bool simpleAssign = opTok && opTok->kind() == SyntaxKind::Eq;
    auto plainIdentTarget = target->asIdent();
    const void* prevWriteTarget = assignmentTargetGreen_;
    if (plainIdentTarget && simpleAssign) assignmentTargetGreen_ = plainIdentTarget->node.greenNode();
    Type* targetT = analyzeExpr(*target);
    assignmentTargetGreen_ = prevWriteTarget;
    if (!isLValue(*target)) {
        errorAtNode(expr.node, "Left side of assignment must be an assignable expression");
    }

    // Narrowing only governs reads; the storage keeps its declared (wider)
    // type. Check assignability against the declared symbol, field, or element
    // type so that e.g. `x = null` and `x.f = null` still work inside
    // `if x != null { }`.
    Type* assignTargetT = targetT;
    if (auto id = target->asIdent()) {
        if (auto* targetInfo = analysis.find(id->node.greenNode())) {
            if (Symbol* sym = targetInfo->resolvedSymbol) {
                if (sym->type) assignTargetT = sym->type;
                if (sym->isConst) {
                    errorAtNode(expr.node, "Cannot assign a new value to '" + asciiOf(sym->name) +
                        "' because it is declared as constant");
                }
            }
        }
    } else if (auto mem = target->asMember()) {
        auto obj = mem->object();
        auto name = mem->memberText();
        Type* objT = obj ? analysis.typeOf(obj->node.greenNode()) : nullptr;
        if (name && objT && objT->structInfo) {
            int idx = objT->structInfo->findFieldIndex(*name);
            if (idx >= 0 && objT->structInfo->fields[idx].type) {
                assignTargetT = objT->structInfo->fields[idx].type;
            }
        }
    } else if (auto sub = target->asSubscript()) {
        auto obj = sub->object();
        Type* objT = obj ? analysis.typeOf(obj->node.greenNode()) : nullptr;
        if (objT && objT->isArray() && objT->inner) {
            assignTargetT = objT->inner;
        }
    }

    Type* valueT = analyzeExprAdapt(*value, assignTargetT);

    if (!assignTargetT->isError() && !valueT->isError()) {
        if (!assignTargetT->assignableFrom(valueT)) {
            errorAtNode(expr.node, "Cannot assign '" + valueT->toString() +
                "' to '" + assignTargetT->toString() + "'");
        }
    }
    // The store lands in the declared slot: retype the target so codegen
    // converts the value to the declared type, and drop the narrowing on the
    // written path.
    if (assignTargetT != targetT) {
        analysis.setType(target->node.greenNode(), assignTargetT);
    }
    invalidateNarrowingsForWrite(*target);
    if (auto id = target->asIdent()) {
        if (auto* targetInfo = analysis.find(id->node.greenNode())) {
            establishAssignmentNarrowing(targetInfo->resolvedSymbol, valueT);
            if (unconditionalPosition_) markAssigned(targetInfo->resolvedSymbol);
        }
    } else if (auto mem = target->asMember()) {
        auto obj = mem->object();
        if (simpleAssign && unconditionalPosition_ && obj && obj->asThis()) {
            auto name = mem->memberText();
            Type* objT = obj ? analysis.typeOf(obj->node.greenNode()) : nullptr;
            if (name && objT && objT->structInfo) {
                int idx = objT->structInfo->findFieldIndex(*name);
                if (idx >= 0) markThisFieldAssigned(&objT->structInfo->fields[idx]);
            }
        }
    }
    return assignTargetT;
}

// Nearest ancestor shared by both class chains (self counts), walking the
// single `extends` chain. Interfaces live off this chain, so the result is
// always a class; null when the two classes share no ancestor.
static StructInfo* nearestCommonBaseClass(StructInfo* a, StructInfo* b) {
    for (StructInfo* s = a; s; s = s->baseInfo) {
        if (b->isSubclassOf(s)) return s;
    }
    return nullptr;
}

Type* Analyzer::unifyValueTypes(Type* a, Type* b) {
    if (a->equals(b)) return a;
    if (Type* c = numericCommonType(a, b)) return c;
    if (a->assignableFrom(b)) return a;
    if (b->assignableFrom(a)) return b;
    // A 'null' side beside a typed side yields the nullable of that type.
    if (a->isNull() != b->isNull()) {
        Type* typed = a->isNull() ? b : a;
        if (!typed->isVoid()) return typeCtx.getOptional(typed);
    }
    // Two sibling class values unify to their nearest common base class; through
    // shared interfaces alone they do not, since interfaces are off the base
    // chain. A nullable side keeps the result nullable.
    bool anyOptional = a->isOptional() || b->isOptional();
    Type* ac = a->isOptional() ? a->inner : a;
    Type* bc = b->isOptional() ? b->inner : b;
    if (ac && bc && ac->isClass() && bc->isClass() &&
        !ac->isInterface() && !bc->isInterface() && ac->structInfo && bc->structInfo) {
        if (StructInfo* base = nearestCommonBaseClass(ac->structInfo, bc->structInfo)) {
            if (Type* baseT = typeCtx.classTypeFor(base)) {
                return anyOptional ? typeCtx.getOptional(baseT) : baseT;
            }
        }
    }
    return nullptr;
}

Type* Analyzer::analyzeTernary(const ast::TernaryExpression& expr) {
    auto cond = expr.condition();
    auto thenE = expr.thenBranch();
    auto elseE = expr.elseBranch();
    Type* condT = cond ? analyzeExpr(*cond) : typeCtx.getError();
    auto analyzeBranch = [&](const ast::Expression& branch, bool conditionHolds) {
        std::vector<NullCheckInfo> narrowings;
        if (cond) collectNarrowings(*cond, conditionHolds, narrowings);
        pushScope();
        for (const auto& info : narrowings) {
            currentScope->narrowedTypes[info.key] = info.narrowedT;
        }
        bool prevUncond = unconditionalPosition_;
        unconditionalPosition_ = false;
        Type* t = analyzeExpr(branch);
        unconditionalPosition_ = prevUncond;
        popScope();
        return t;
    };
    Type* thenT = thenE ? analyzeBranch(*thenE, true) : typeCtx.getError();
    Type* elseT = elseE ? analyzeBranch(*elseE, false) : typeCtx.getError();
    if (cond && !condT->isError() && !condT->isBool()) {
        errorAtNode(cond->node, "Ternary condition must be 'bool', got '" + condT->toString() + "'");
    }
    if (thenT->isError() || elseT->isError()) return typeCtx.getError();
    // Adapt polymorphic int literal in one branch toward the other branch's type.
    if (thenE) tryAdaptIntegerLiteral(*thenE, elseT);
    if (elseE) tryAdaptIntegerLiteral(*elseE, thenT);
    if (thenE) { Type* upd = analysis.typeOf(thenE->node.greenNode()); if (upd) thenT = upd; }
    if (elseE) { Type* upd = analysis.typeOf(elseE->node.greenNode()); if (upd) elseT = upd; }
    if (Type* unified = unifyValueTypes(thenT, elseT)) return unified;
    errorAtNode(expr.node, "The two results of this '?:' have incompatible types '" +
        thenT->toString() + "' and '" + elseT->toString() +
        "'; both branches must produce one common type.");
    return typeCtx.getError();
}

Type* Analyzer::analyzeNullCoalesce(const ast::NullCoalesceExpression& expr) {
    auto left = expr.left();
    auto right = expr.right();
    if (!left || !right) return typeCtx.getError();
    Type* l = analyzeExpr(*left);
    bool prevUncond = unconditionalPosition_;
    unconditionalPosition_ = false;
    Type* r = analyzeExpr(*right);
    unconditionalPosition_ = prevUncond;
    if (l->isError() || r->isError()) return typeCtx.getError();
    // `?\?` is a presence test on the left's declared storage type, so a
    // binding declared optional keeps it legal even after narrowing proved the
    // value non-null; the check is then constant and yields the value.
    l = presenceOperandType(*left, l);
    if (!l->isOptional() || !l->inner) {
        errorAtNode(expr.node, "Left of '?\?' must be a nullable value, got '" + l->toString() + "'");
        return typeCtx.getError();
    }
    Type* unwrapped = l->inner;
    // The result is the non-null type when the right side fits it, and stays
    // nullable when the right side is itself nullable.
    if (unwrapped->assignableFrom(r)) return unwrapped;
    if (l->assignableFrom(r)) return l;
    errorAtNode(expr.node, "Right of '?\?' has type '" + r->toString() +
        "' which is not compatible with '" + unwrapped->toString() + "'");
    return typeCtx.getError();
}

Type* Analyzer::analyzeNew(const ast::NewExpression& expr) {
    auto tr = expr.typeReference();
    if (!tr) return typeCtx.getError();
    auto typeName = tr->nameText();
    if (!typeName) return typeCtx.getError();

    if (expr.isArrayNew()) {
        auto sizes = expr.arraySizeExpressions();
        int unsized = expr.arrayUnsizedTrailingCount();
        Type* elem = resolveTypeReference(*tr);
        if (elem->isError()) {
            for (auto& sz : sizes) analyzeExpr(sz);
            return typeCtx.getError();
        }
        if (elem->isVoid()) {
            errorAtNode(tr->node, "Cannot create an array of void");
            for (auto& sz : sizes) analyzeExpr(sz);
            return typeCtx.getError();
        }
        if (sizes.empty()) {
            errorAtNode(expr.node, "'new " + elem->toString() +
                "[...]' requires at least one sized dimension. Use 'new " +
                elem->toString() + "[size]' for a 1-D array, or 'new " +
                elem->toString() + "[size][]' for a partially-allocated grid.");
            Type* arrT = typeCtx.getArray(elem);
            analysis.setType(expr.node.greenNode(), arrT);
            return arrT;
        }
        // When `unsized == 0`, every level is allocated, so the deepest slots
        // hold values of type T directly. T must therefore satisfy the
        // element-nullability rule, unless a fill loop right after the
        // declaration proves every slot is written before any read. When
        // `unsized > 0`, the slots at the deepest allocated level hold
        // nullable inner arrays (which are defaultable as `null`), so T
        // itself is never zero-initialized and doesn't need to be defaultable
        // here.
        Type* slotElem;
        if (unsized == 0) {
            bool singleDim = sizes.size() == 1;
            bool referenceElem = elem->isClass() || elem->isArray() ||
                                 elem->isString() || elem->isExternal();
            bool provenFilled = singleDim && referenceElem &&
                fillLoopProvenNews_.count(expr.node.greenNode()) > 0;
            std::optional<std::u16string> fillExampleName;
            if (singleDim) {
                auto named = arrayNewDeclNames_.find(expr.node.greenNode());
                fillExampleName = named != arrayNewDeclNames_.end()
                    ? named->second : std::u16string(u"items");
            }
            if (!provenFilled && !validateArrayElement(elem, tr->node, fillExampleName)) {
                for (auto& sz : sizes) analyzeExpr(sz);
                return typeCtx.getError();
            }
            slotElem = elem;
        } else {
            Type* innerArr = elem;
            for (int i = 0; i < unsized; ++i) innerArr = typeCtx.getArray(innerArr);
            slotElem = typeCtx.getOptional(innerArr);
        }
        for (auto& sz : sizes) {
            Type* sizeT = analyzeExpr(sz);
            if (!sizeT->isError() && !sizeT->isInteger()) {
                errorAtNode(sz.node, "Array size must be an integer, got '" + sizeT->toString() + "'");
            }
        }
        // Result type: Array applied `sizes.size()` times to the deepest slot
        // type (which already folds in the unsized-tail `?` when applicable).
        Type* arrT = slotElem;
        for (size_t i = 0; i < sizes.size(); ++i) {
            arrT = typeCtx.getArray(arrT);
        }
        analysis.setType(expr.node.greenNode(), arrT);
        return arrT;
    }

    if (tr->isOptional()) {
        errorAtNode(tr->node, "'new' cannot construct an optional type");
    }

    Type* t = resolveTypeReference(*tr);
    if (t->isError()) {
        for (auto& a : expr.arguments()) analyzeExpr(a);
        return typeCtx.getError();
    }
    if (!t->isClass()) {
        if (t->isStruct()) {
            errorAtNode(expr.node, "'new' is only valid for classes; '" +
                asciiOf(*typeName) + "' is a struct");
        } else {
            errorAtNode(expr.node, "'new' requires a class type, got '" + t->toString() + "'");
        }
        for (auto& a : expr.arguments()) analyzeExpr(a);
        return typeCtx.getError();
    }
    analysis.setType(expr.node.greenNode(), t);

    if (t->structInfo && t->structInfo->isInterface) {
        errorAtNode(expr.node, "Cannot create an instance of interface '" + asciiOf(*typeName) +
            "'; instantiate a class that implements it instead");
    } else if (t->structInfo && t->structInfo->isAbstract) {
        errorAtNode(expr.node, "Cannot create an instance of abstract class '" +
            asciiOf(*typeName) + "'; instantiate a concrete subclass instead");
    }

    std::vector<const MethodInfo*> ctorCands;
    for (const auto& m : t->structInfo->methods) {
        if (m.isConstructor && m.symbol) ctorCands.push_back(&m);
    }

    auto args = expr.arguments();
    if (ctorCands.size() > 1 || (!ctorCands.empty() && callUsesNamedArguments(args))) {
        CallShape shape = analyzeCallShape(args);
        std::vector<OverloadCandidate> candidates;
        for (const MethodInfo* mi : ctorCands) {
            candidates.push_back({mi->symbol, mi,
                isMemberAccessAllowed(mi->visibility,
                                      mi->definingClass ? mi->definingClass : t->structInfo)});
        }
        OverloadChoice choice = resolveOverloadedCall(
            candidates, shape, expr.node, asciiOf(*typeName), "Constructor");
        if (!choice.failed) {
            if (!choice.accessible && choice.method) {
                checkConstructorAccess(expr.node, t->structInfo, *choice.method);
            }
            analysis.setMethodSymbol(expr.node.greenNode(), choice.symbol);
            checkResolvedCallArguments(shape, choice, expr.node.greenNode());
        }
    } else if (!ctorCands.empty()) {
        checkConstructorAccess(expr.node, t->structInfo, *ctorCands.front());
        Symbol* ctor = ctorCands.front()->symbol;
        analysis.setMethodSymbol(expr.node.greenNode(), ctor);
        size_t req = requiredArgCount(ctor);
        if (args.size() < req || args.size() > ctor->paramTypes.size()) {
            errorAtNode(expr.node, "Constructor '" + asciiOf(*typeName) + "' expects " +
                std::to_string(req) +
                (req == ctor->paramTypes.size() ? "" : "-" + std::to_string(ctor->paramTypes.size())) +
                " argument(s), got " + std::to_string(args.size()));
        }
        size_t n = std::min(args.size(), ctor->paramTypes.size());
        for (size_t i = 0; i < n; ++i) {
            Type* paramT = ctor->paramTypes[i];
            Type* argT = analyzeExprAdapt(args[i], paramT);
            if (!paramT->assignableFrom(argT)) {
                errorAtNode(args[i].node, "Argument " + std::to_string(i + 1) +
                    ": expected '" + paramT->toString() + "', got '" + argT->toString() + "'");
            }
        }
        for (size_t i = n; i < args.size(); ++i) analyzeExpr(args[i]);
    } else if (!args.empty()) {
        errorAtNode(expr.node, "Class '" + asciiOf(*typeName) + "' has no constructor; use 'new " +
            asciiOf(*typeName) + "()'");
        for (auto& a : args) analyzeExpr(a);
    }
    clearNarrowingsForArguments(args);
    return t;
}

Type* Analyzer::analyzeParen(const ast::ParenExpression& expr) {
    if (auto inner = expr.inner()) return analyzeExpr(*inner);
    return typeCtx.getError();
}

Type* Analyzer::analyzeTry(const ast::TryExpression& expr) {
    auto operand = expr.operand();
    if (!operand) return typeCtx.getError();
    if (operand->node.kind() != SyntaxKind::CallExpr) {
        errorAtNode(expr.node, "'try' must directly prefix a single call, "
            "e.g. 'try f(x)' or 'try obj.m()'.");
        return analyzeExpr(*operand);
    }
    return analyzeExpr(*operand);
}

void Analyzer::analyzeThrowStmt(const ast::ThrowStatement& stmt) {
    auto value = stmt.value();
    if (!value) { flowTerminated_ = true; return; }
    Type* t = analyzeExpr(*value);
    flowTerminated_ = true;
    if (t->isError()) return;
    bool isErrorSubclass = t->isClass() && t->structInfo && errorClassInfo_ &&
        t->structInfo->isSubclassOf(errorClassInfo_);
    if (!isErrorSubclass) {
        errorAtNode(value->node, "Cannot throw a value of type '" + t->toString() +
            "'; only 'Error' or a subclass of it can be thrown.");
    }
}

void Analyzer::analyzeRethrowStmt(const ast::RethrowStatement& stmt) {
    if (!inCatchClause) {
        errorAtNode(stmt.node, "'rethrow' can only be used inside a 'catch' block.");
    }
    flowTerminated_ = true;
}

namespace {

std::optional<int64_t> switchIntLabelValue(const ast::Expression& e) {
    // Any integer-literal form names a constant: an int or long literal by its
    // value, a char literal by its codepoint. This lets `'A'`, `65`, and `65L`
    // name the same label, so mixing them is a duplicate.
    return integerConstantValue(e);
}

bool isNullLabel(const ast::Expression& e) {
    if (auto lit = e.asLiteral()) return lit->literalKind() == SyntaxKind::KwNull;
    return false;
}

// A string label's decoded value, so two spellings of the same string (for
// example "\t" and a literal tab) collide as a duplicate.
bool stringLabelText(const ast::Expression& e, std::u16string& out) {
    if (auto lit = e.asLiteral()) {
        if (lit->literalKind() == SyntaxKind::StringLiteral) {
            if (auto tok = lit->token()) { out = decodeStringLiteral(tok->tokenText()); return true; }
        }
    }
    return false;
}

}  // namespace

void Analyzer::analyzeSwitchStmt(const ast::SwitchStatement& stmt) {
    analyzeSwitchArms(stmt.scrutinee(), stmt.arms(), stmt.node, /*requireValue=*/false);
}

Type* Analyzer::analyzeSwitchExpr(const ast::SwitchExpression& expr) {
    return analyzeSwitchArms(expr.scrutinee(), expr.arms(), expr.node, /*requireValue=*/true);
}

Type* Analyzer::analyzeSwitchArms(const std::optional<ast::Expression>& scrutinee,
                                  const std::vector<ast::SwitchArm>& arms,
                                  const SyntaxNode& diagNode, bool requireValue) {
    Type* scrutType = scrutinee ? analyzeExpr(*scrutinee) : typeCtx.getError();
    bool nullable = scrutType->isOptional();
    Type* inner = nullable ? scrutType->inner : scrutType;

    bool hasTypeArm = false;
    for (auto& arm : arms) {
        if (arm.isTypeArm()) { hasTypeArm = true; break; }
    }

    bool classScrut = inner && !inner->isError() && inner->isClass() && inner->structInfo;
    bool scrutOk = inner && !inner->isError() &&
                   (inner->isEnum() || inner->isInteger() || inner->isString());
    bool typeSwitch = hasTypeArm && classScrut;
    if (scrutinee && !scrutType->isError()) {
        if (classScrut && !hasTypeArm) {
            errorAtNode(scrutinee->node, "A switch over a class value needs at least one "
                "'is Type ->' arm.");
        } else if (!classScrut && !scrutOk) {
            errorAtNode(scrutinee->node, "Cannot switch on a value of type '" + scrutType->toString() +
                "'; switch supports enum, integer, string, and class values.");
        }
    }

    bool hasDefault = false;
    bool nullCovered = false;
    std::vector<int64_t> seenInts;
    std::vector<std::u16string> seenStrings;
    std::vector<int64_t> coveredEnum;
    std::vector<StructInfo*> armClasses;
    std::vector<ast::Expression> valueExprs;
    bool sawValueBlock = false;

    // Each arm starts from the state after the scrutinee. A local assigned in
    // every arm of a switch (which the checker requires to be exhaustive) is
    // assigned after it; a terminated arm drops out of the join. The narrowing
    // facts join the same way: a path stays narrowed after the switch only when
    // every arm that falls through proves it.
    AssignmentFlow switchEntry = snapshotAssignment();
    NarrowingSnapshot entryNarrowing = captureNarrowings();
    NarrowingFacts entryFacts = flattenNarrowings();
    std::vector<AssignmentFlow> armFlows;
    std::vector<NarrowingFacts> armNarrowings;
    auto analyzeArmBody = [&](const ast::SwitchArm& arm) {
        if (auto bn = arm.bodyBlockNode()) {
            if (requireValue) {
                sawValueBlock = true;
                errorAtNode(*bn, "A switch used as a value must use expression arms, not '{ }' blocks.");
            }
            if (auto blk = ast::Block::cast(*bn)) {
                pushScope();
                analyzeStatements(blk->statements());
                armNarrowings.push_back(flattenNarrowings());
                popScope();
            } else {
                armNarrowings.push_back(flattenNarrowings());
            }
        } else if (auto be = arm.bodyExpr()) {
            analyzeExpr(*be);
            armNarrowings.push_back(flattenNarrowings());
            if (requireValue) valueExprs.push_back(*be);
        } else {
            armNarrowings.push_back(flattenNarrowings());
        }
        armFlows.push_back(snapshotAssignment());
    };

    for (auto& arm : arms) {
        restoreAssignment(switchEntry);
        restoreNarrowings(entryNarrowing);
        unconditionalPosition_ = true;
        if (arm.isDefault()) {
            if (hasDefault) errorAtNode(arm.node, "A switch can have only one 'default' arm.");
            hasDefault = true;
        } else if (arm.isTypeArm()) {
            auto tr = arm.typeReference();
            Type* armT = tr ? resolveTypeReference(*tr) : typeCtx.getError();
            SyntaxNode diag = tr ? tr->node : arm.node;
            StructInfo* armClass = nullptr;
            if (!classScrut) {
                if (scrutOk) {
                    errorAtNode(diag, "An 'is' arm needs a class switch value, but this switch "
                        "is over '" + scrutType->toString() + "'.");
                }
            } else if (!armT->isError()) {
                armClass = checkTypeArmTarget(scrutType, inner, armT, diag);
            }
            if (armClass) {
                for (StructInfo* prev : armClasses) {
                    if (armClass->isSubclassOrConforms(prev)) {
                        errorAtNode(diag, "This 'is " + asciiOf(armClass->name) + "' arm is "
                            "unreachable: the earlier 'is " + asciiOf(prev->name) + "' arm "
                            "already matches every '" + asciiOf(armClass->name) + "'. Move it "
                            "before the broader arm, or remove it.");
                        break;
                    }
                }
                armClasses.push_back(armClass);
            }
            pushScope();
            if (auto bindTok = arm.bindingNameToken()) {
                Type* bindT = armClass ? armT : typeCtx.getError();
                Symbol* binding = makeSymbol(SymbolKind::Variable,
                    arm.bindingNameText().value_or(std::u16string{}), bindT, bindTok->startOffset());
                binding->isConst = true;
                binding->isBorrowedBinding = true;
                currentScope->define(binding);
                analysis.setSymbol(arm.node.greenNode(), binding);
            }
            analyzeArmBody(arm);
            popScope();
            continue;
        } else {
            bool mixReported = false;
            for (auto& label : arm.labels()) {
                if (isNullLabel(label)) {
                    analysis.setType(label.node.greenNode(), typeCtx.getNull());
                    if (!nullable) {
                        errorAtNode(label.node, "'null' is only a valid label when the switch value is nullable.");
                    } else if (nullCovered) {
                        errorAtNode(label.node, "Duplicate 'null' label.");
                    } else {
                        nullCovered = true;
                    }
                    continue;
                }
                if (typeSwitch) {
                    if (!mixReported) {
                        errorAtNode(label.node, "Cannot mix value labels with 'is' arms in the "
                            "same switch.");
                        mixReported = true;
                    }
                    continue;
                }
                if (!scrutOk) continue;
                if (inner->isEnum()) {
                    std::optional<int64_t> value;
                    if (auto id = label.asIdent()) {
                        auto lname = id->nameText();
                        if (lname && inner->structInfo) {
                            for (auto& m : inner->structInfo->enumMembers) {
                                if (m.name == *lname) { value = m.value; break; }
                            }
                        }
                        if (value) {
                            analysis.setType(label.node.greenNode(), inner);
                            analysis.setEnumConstant(label.node.greenNode(), *value);
                        } else {
                            errorAtNode(label.node, "'" + asciiOf(lname.value_or(std::u16string{})) +
                                "' is not a member of enum '" + inner->toString() + "'.");
                        }
                    } else {
                        Type* lt = analyzeExpr(label);
                        if (!lt->isError() && !lt->equals(inner)) {
                            errorAtNode(label.node, "Switch label of type '" + lt->toString() +
                                "' does not match the switch value of type '" + inner->toString() + "'.");
                        }
                        if (auto ec = analysis.enumConstantOf(label.node.greenNode())) value = *ec;
                    }
                    if (value) {
                        bool dup = false;
                        for (int64_t v : coveredEnum) if (v == *value) { dup = true; break; }
                        if (dup) errorAtNode(label.node, "Duplicate switch label.");
                        else coveredEnum.push_back(*value);
                    }
                } else if (inner->isInteger()) {
                    Type* lt = analyzeExpr(label);
                    if (auto v = switchIntLabelValue(label)) {
                        // An integer-literal label adapts to the scrutinee: a char
                        // literal like an int/long literal, its value fit-checked and
                        // the node retyped to the scrutinee's width.
                        if (auto cl = label.asLiteral();
                            cl && cl->literalKind() == SyntaxKind::CharLiteral) {
                            tryAdaptCharLiteral(label, inner);
                        } else {
                            adaptIntegerLiteralLabel(label, inner);
                        }
                        Type* adapted = analysis.typeOf(label.node.greenNode());
                        if (!adapted || !adapted->isError()) {
                            bool dup = false;
                            for (int64_t s : seenInts) if (s == *v) { dup = true; break; }
                            if (dup) errorAtNode(label.node, "Duplicate switch label '" + std::to_string(*v) + "'.");
                            else seenInts.push_back(*v);
                        }
                    } else if (!lt->isError()) {
                        if (!inner->assignableFrom(lt) && !lt->assignableFrom(inner)) {
                            errorAtNode(label.node, "Switch label of type '" + lt->toString() +
                                "' does not match the switch value of type '" + inner->toString() + "'.");
                        } else {
                            errorAtNode(label.node, "Switch labels for an integer switch must be integer constants.");
                        }
                    }
                } else {  // string
                    Type* lt = analyzeExpr(label);
                    std::u16string text;
                    if (!lt->isError() && !lt->isString()) {
                        errorAtNode(label.node, "Switch label of type '" + lt->toString() +
                            "' does not match the switch value of type 'string'.");
                    } else if (stringLabelText(label, text)) {
                        bool dup = false;
                        for (auto& s : seenStrings) if (s == text) { dup = true; break; }
                        if (dup) errorAtNode(label.node, "Duplicate switch label.");
                        else seenStrings.push_back(text);
                    } else if (!lt->isError()) {
                        errorAtNode(label.node, "Switch labels for a string switch must be string literals.");
                    }
                }
            }
        }
        analyzeArmBody(arm);
    }

    if (scrutOk && !hasDefault) {
        if (inner->isEnum() && inner->structInfo) {
            std::vector<std::u16string> missing;
            for (auto& m : inner->structInfo->enumMembers) {
                bool covered = false;
                for (int64_t v : coveredEnum) if (v == m.value) { covered = true; break; }
                if (!covered) missing.push_back(m.name);
            }
            if (!missing.empty()) {
                std::string list;
                for (size_t i = 0; i < missing.size(); ++i) {
                    if (i) list += ", ";
                    list += "'" + asciiOf(missing[i]) + "'";
                }
                errorAtNode(diagNode, "This switch does not handle enum '" + inner->toString() +
                    "' member(s) " + list + ". Add them or a 'default' arm.");
            }
        } else if (inner->isInteger() || inner->isString()) {
            errorAtNode(diagNode, "A switch on '" + inner->toString() +
                "' must have a 'default' arm.");
        }
    }
    if (typeSwitch && !hasDefault) {
        StructInfo* root = inner->structInfo;
        std::string rootName = inner->toString();
        if (root->isInterface) {
            errorAtNode(diagNode, "A switch over '" + rootName + "' must have a 'default' arm: "
                "interfaces are open, so any class anywhere may implement '" + rootName + "'.");
        } else if (!root->isSealed) {
            errorAtNode(diagNode, "A switch over '" + rootName + "' must have a 'default' arm "
                "because '" + rootName + "' is not sealed.");
        } else if (!root->isAbstract) {
            errorAtNode(diagNode, "A switch over '" + rootName + "' must have a 'default' arm: '" +
                rootName + "' is concrete, so a value may be a plain '" + rootName +
                "' that no subclass arm matches.");
        } else {
            std::vector<std::u16string> missing;
            for (StructInfo* sub : root->directSubclasses) {
                bool covered = false;
                for (StructInfo* a : armClasses) {
                    if (sub->isSubclassOrConforms(a)) { covered = true; break; }
                }
                if (!covered) missing.push_back(sub->name);
            }
            if (!missing.empty()) {
                std::string list;
                for (size_t i = 0; i < missing.size(); ++i) {
                    if (i) list += ", ";
                    list += "'" + asciiOf(missing[i]) + "'";
                }
                errorAtNode(diagNode, "This switch does not handle subclass(es) " + list +
                    " of sealed class '" + rootName + "'. Add arms for them or a 'default' arm.");
            }
        }
    }
    if ((scrutOk || typeSwitch) && nullable && !nullCovered && !hasDefault) {
        errorAtNode(diagNode, "This switch value can be null but no arm handles it. "
            "Add a 'null ->' arm or a 'default' arm.");
    }

    if (!armFlows.empty()) restoreAssignment(joinAssignment(armFlows));
    else restoreAssignment(switchEntry);

    restoreNarrowings(entryNarrowing);
    std::vector<NarrowingFacts> survivors;
    for (size_t i = 0; i < armFlows.size(); ++i) {
        if (!armFlows[i].terminated) survivors.push_back(std::move(armNarrowings[i]));
    }
    applyNarrowingJoin(entryFacts, survivors);

    if (!requireValue) return typeCtx.getPrimitive(TypeKind::Void);

    if (sawValueBlock) return typeCtx.getError();
    if (valueExprs.empty()) {
        errorAtNode(diagNode, "A switch used as a value needs at least one arm.");
        return typeCtx.getError();
    }
    Type* result = nullptr;
    for (auto& ve : valueExprs) {
        Type* t = analysis.typeOf(ve.node.greenNode());
        if (!t || t->isError()) return typeCtx.getError();
        if (!result) { result = t; continue; }
        tryAdaptIntegerLiteral(ve, result);
        if (Type* u = analysis.typeOf(ve.node.greenNode())) t = u;
        if (Type* unified = unifyValueTypes(result, t)) { result = unified; continue; }
        errorAtNode(diagNode, "Switch arms produce incompatible types '" + result->toString() +
            "' and '" + t->toString() + "'.");
        return typeCtx.getError();
    }
    if (result && result->isVoid()) {
        errorAtNode(diagNode, "A switch used as a value must produce a value in every arm.");
        return typeCtx.getError();
    }
    for (auto& ve : valueExprs) tryAdaptIntegerLiteral(ve, result);
    return result ? result : typeCtx.getError();
}

// Array literal `[a, b, c]` with no target type: first-element-wins.
// The first element's type drives the rest; subsequent elements are adapted
// against that element type via analyzeExprAdapt so int/char literals narrow.
Type* Analyzer::analyzeArrayLiteral(const ast::ArrayLiteralExpression& expr) {
    auto elems = expr.elements();
    if (elems.empty()) {
        errorAtNode(expr.node,
            "Cannot infer element type from an empty array literal; "
            "annotate the type, e.g. 'int[] xs = [];'.");
        return typeCtx.getError();
    }
    Type* elemT = analyzeExpr(elems[0]);
    if (elemT->isError()) {
        for (size_t i = 1; i < elems.size(); ++i) analyzeExpr(elems[i]);
        return typeCtx.getError();
    }
    if (elemT->isNull()) {
        errorAtNode(elems[0].node,
            "Cannot infer element type from 'null' alone; "
            "annotate the array's element type, e.g. 'T?[] xs = [null, ...];'.");
        for (size_t i = 1; i < elems.size(); ++i) analyzeExpr(elems[i]);
        return typeCtx.getError();
    }
    for (size_t i = 1; i < elems.size(); ++i) {
        Type* actual = analyzeExprAdapt(elems[i], elemT);
        if (actual->isError()) continue;
        if (!elemT->assignableFrom(actual)) {
            errorAtNode(elems[i].node, "Element " + std::to_string(i + 1) +
                ": expected '" + elemT->toString() + "', got '" +
                actual->toString() + "'.");
        }
    }
    return typeCtx.getArray(elemT);
}

// Array literal with a target type: each element is adapted to the target's
// element type. The target may be `T[]` or `T[]?` (nullable holder).
Type* Analyzer::analyzeArrayLiteralAdapt(const ast::ArrayLiteralExpression& expr,
                                         Type* target) {
    if (!target || target->isError()) {
        return analyzeArrayLiteral(expr);
    }
    Type* arrayT = target;
    if (arrayT->isOptional() && arrayT->inner) arrayT = arrayT->inner;
    if (!arrayT->isArray() || !arrayT->inner) {
        errorAtNode(expr.node,
            "Cannot assign array literal to non-array type '" +
            target->toString() + "'.");
        for (auto& e : expr.elements()) analyzeExpr(e);
        return typeCtx.getError();
    }
    Type* elemT = arrayT->inner;
    auto elems = expr.elements();
    for (size_t i = 0; i < elems.size(); ++i) {
        Type* actual = analyzeExprAdapt(elems[i], elemT);
        if (actual->isError()) continue;
        if (!elemT->assignableFrom(actual)) {
            errorAtNode(elems[i].node, "Element " + std::to_string(i + 1) +
                ": expected '" + elemT->toString() + "', got '" +
                actual->toString() + "'.");
        }
    }
    // Return the non-nullable array form; the optional wrap (if any) is
    // applied at the use site via the target's declared type.
    return arrayT;
}

static bool fieldHasDefaultValue(const FieldInfo& f);

// A `{...}` literal with no target type: nothing to infer against.
Type* Analyzer::analyzeStructLiteral(const ast::StructLiteralExpression& expr) {
    for (auto& f : expr.fields()) {
        if (auto v = f.value()) analyzeExpr(*v);
    }
    errorAtNode(expr.node,
        "Cannot tell which struct this '{...}' literal builds. Annotate the target "
        "with a struct type or use a constructor, e.g. 'Point p = {x: 1, y: 2};'.");
    return typeCtx.getError();
}

// A struct literal with a target type: every field is set by name against the
// target struct's declared fields.
Type* Analyzer::analyzeStructLiteralAdapt(const ast::StructLiteralExpression& expr, Type* target) {
    if (!target || target->isError()) {
        return analyzeStructLiteral(expr);
    }
    Type* structT = target;
    if (structT->isOptional() && structT->inner) structT = structT->inner;
    if (!structT->isStruct() || !structT->structInfo) {
        errorAtNode(expr.node, "Cannot build '" + target->toString() +
            "' from a '{...}' literal; that type is not a struct.");
        for (auto& f : expr.fields()) if (auto v = f.value()) analyzeExpr(*v);
        return typeCtx.getError();
    }
    analysis.setType(expr.node.greenNode(), structT);
    StructInfo* si = structT->structInfo;
    std::vector<bool> provided(si->fields.size(), false);
    for (auto& f : expr.fields()) {
        auto fname = f.nameText();
        auto valueExpr = f.value();
        if (!fname) {
            if (valueExpr) analyzeExpr(*valueExpr);
            continue;
        }
        int idx = si->findFieldIndex(*fname);
        if (idx < 0) {
            errorAtNode(f.node, "Struct '" + asciiOf(si->name) + "' has no field named '" +
                asciiOf(*fname) + "'.");
            if (valueExpr) analyzeExpr(*valueExpr);
            continue;
        }
        if (provided[idx]) {
            errorAtNode(f.node, "Field '" + asciiOf(*fname) +
                "' is set more than once in this struct literal.");
            if (valueExpr) analyzeExpr(*valueExpr);
            continue;
        }
        provided[idx] = true;
        const FieldInfo& fi = si->fields[idx];
        checkMemberAccess(f.node, *fname, fi.visibility, fi.definingClass);
        if (!valueExpr) continue;
        Type* fieldT = fi.type;
        Type* valueT = analyzeExprAdapt(*valueExpr, fieldT);
        if (!valueT->isError() && fieldT && !fieldT->isError() &&
            !fieldT->assignableFrom(valueT)) {
            errorAtNode(valueExpr->node, "Field '" + asciiOf(*fname) + "': expected '" +
                fieldT->toString() + "', got '" + valueT->toString() + "'.");
        }
    }
    for (size_t i = 0; i < si->fields.size(); ++i) {
        if (provided[i]) continue;
        const FieldInfo& fi = si->fields[i];
        if (fieldHasDefaultValue(fi)) continue;
        errorAtNode(expr.node, "Struct literal for '" + asciiOf(si->name) +
            "' is missing the required field '" + asciiOf(fi.name) +
            "'. Add '" + asciiOf(fi.name) + ": ...' or give the field a default value.");
    }
    return structT;
}

// `StructName(args)`: a by-value struct built through its constructor. Resolution
// mirrors class-constructor resolution (overloads, named and optional arguments)
// with no heap allocation.
Type* Analyzer::analyzeStructConstructorCall(const ast::CallExpression& expr, Type* structType,
                                             const std::u16string& typeName) {
    auto args = expr.arguments();
    analysis.setType(expr.node.greenNode(), structType);
    StructInfo* si = structType->structInfo;
    if (!si) return typeCtx.getError();
    if (si->isTemplate) {
        errorAtNode(expr.node, "Cannot tell which instantiation of the generic struct '" +
            asciiOf(typeName) + "' to build; write the type arguments explicitly, e.g. '" +
            asciiOf(typeName) + "<int, string>(...)'.");
        for (auto& a : args) analyzeExpr(a);
        return structType;
    }
    std::vector<const MethodInfo*> ctorCands;
    for (const auto& m : si->methods) {
        if (m.isConstructor && m.symbol) ctorCands.push_back(&m);
    }
    if (ctorCands.empty()) {
        errorAtNode(expr.node, "Struct '" + asciiOf(typeName) +
            "' has no constructor to call; build it with an aggregate literal, e.g. '" +
            asciiOf(typeName) + " value = {...};', or add a 'constructor(...)'.");
        for (auto& a : args) analyzeExpr(a);
        return structType;
    }
    if (ctorCands.size() > 1 || callUsesNamedArguments(args)) {
        CallShape shape = analyzeCallShape(args);
        std::vector<OverloadCandidate> candidates;
        for (const MethodInfo* mi : ctorCands) {
            candidates.push_back({mi->symbol, mi,
                isMemberAccessAllowed(mi->visibility,
                                      mi->definingClass ? mi->definingClass : si)});
        }
        OverloadChoice choice = resolveOverloadedCall(
            candidates, shape, expr.node, asciiOf(typeName), "Constructor");
        if (!choice.failed) {
            if (!choice.accessible && choice.method) {
                checkConstructorAccess(expr.node, si, *choice.method);
            }
            analysis.setMethodSymbol(expr.node.greenNode(), choice.symbol);
            checkResolvedCallArguments(shape, choice, expr.node.greenNode());
        }
        return structType;
    }
    checkConstructorAccess(expr.node, si, *ctorCands.front());
    Symbol* ctor = ctorCands.front()->symbol;
    analysis.setMethodSymbol(expr.node.greenNode(), ctor);
    size_t req = requiredArgCount(ctor);
    if (args.size() < req || args.size() > ctor->paramTypes.size()) {
        errorAtNode(expr.node, "Constructor '" + asciiOf(typeName) + "' expects " +
            std::to_string(req) +
            (req == ctor->paramTypes.size() ? "" : "-" + std::to_string(ctor->paramTypes.size())) +
            " argument(s), got " + std::to_string(args.size()));
    }
    size_t n = std::min(args.size(), ctor->paramTypes.size());
    for (size_t i = 0; i < n; ++i) {
        Type* paramT = ctor->paramTypes[i];
        Type* argT = analyzeExprAdapt(args[i], paramT);
        if (!paramT->assignableFrom(argT)) {
            errorAtNode(args[i].node, "Argument " + std::to_string(i + 1) +
                ": expected '" + paramT->toString() + "', got '" + argT->toString() + "'");
        }
    }
    for (size_t i = n; i < args.size(); ++i) analyzeExpr(args[i]);
    return structType;
}

bool Analyzer::isLValue(const ast::Expression& expr) const {
    SyntaxKind k = expr.kind();
    if (k == SyntaxKind::IdentExpr || k == SyntaxKind::SubscriptExpr ||
        k == SyntaxKind::ThisExpr) {
        return true;
    }
    if (k != SyntaxKind::MemberExpr) return false;
    // A member write through a class reference mutates the heap object, so any
    // object expression works. A struct member write needs an addressable
    // struct, so the object must itself be an lvalue.
    auto member = expr.asMember();
    auto obj = member ? member->object() : std::nullopt;
    if (!obj) return false;
    Type* objT = analysis.typeOf(obj->node.greenNode());
    if (objT && objT->isStruct()) return isLValue(*obj);
    return true;
}

static bool fieldHasDefaultValue(const FieldInfo& f) {
    if (!f.declaration) return false;
    auto fieldNode = SyntaxNode::makeRoot(f.declaration);
    auto fd = ast::FieldDecl::cast(*fieldNode);
    return fd && fd->defaultValue().has_value();
}

bool Analyzer::isDefaultable(Type* t) const {
    std::unordered_set<const StructInfo*> visiting;
    return isDefaultable(t, visiting);
}

bool Analyzer::isDefaultable(Type* t, std::unordered_set<const StructInfo*>& visiting) const {
    if (!t) return false;
    if (t->isPrimitive()) return true;
    if (t->isOptional()) return true;
    if (t->isStruct()) {
        if (!t->structInfo) return false;
        if (!visiting.insert(t->structInfo).second) return true;
        for (auto& f : t->structInfo->fields) {
            if (isDefaultable(f.type, visiting)) continue;
            if (fieldHasDefaultValue(f)) continue;
            visiting.erase(t->structInfo);
            return false;
        }
        visiting.erase(t->structInfo);
        return true;
    }
    return false;
}

namespace {

// A field stores a struct inline when its type is that struct or an optional of it.
// Classes, arrays, and strings are references and never nest a struct's storage.
StructInfo* byValueFieldStruct(Type* t) {
    if (t && t->isOptional()) t = t->inner;
    return t && t->isStruct() ? t->structInfo : nullptr;
}

struct StructCycleStep {
    StructInfo* owner;
    const FieldInfo* field;
    StructInfo* target;
};

// Depth-first search for a by-value field path from `origin` back to itself.
// Instantiations count as their template, so `Wrap<T>` inside 'struct Wrap<T>'
// closes the cycle. `explored` bounds the walk on graphs with shared members.
bool findPathBackTo(StructInfo* origin, StructInfo* current,
                    std::vector<StructCycleStep>& path,
                    std::unordered_set<StructInfo*>& explored) {
    for (const auto& f : current->fields) {
        StructInfo* next = byValueFieldStruct(f.type);
        if (!next) continue;
        path.push_back({current, &f, next});
        StructInfo* authority = next->templateOf ? next->templateOf : next;
        if (authority == origin) return true;
        if (explored.insert(next).second &&
            findPathBackTo(origin, next, path, explored)) {
            return true;
        }
        path.pop_back();
    }
    return false;
}

}  // namespace

void Analyzer::checkStructValueCycles() {
    if (!astRoot) return;
    for (auto& sd : astRoot->structs()) {
        Type* t = analysis.typeOf(sd.node.greenNode());
        if (!t || !t->structInfo) continue;
        StructInfo* origin = t->structInfo;

        std::vector<StructCycleStep> path;
        std::unordered_set<StructInfo*> explored;
        if (!findPathBackTo(origin, origin, path, explored)) continue;

        std::string msg = "Struct '" + asciiOf(origin->name) + "' contains itself by value: ";
        for (size_t i = 0; i < path.size(); ++i) {
            if (i > 0) msg += i + 1 == path.size() ? ", and " : ", ";
            msg += "field '" + asciiOf(path[i].field->name) + "' of '" +
                asciiOf(path[i].owner->name) + "' has type '" +
                (path[i].field->type ? path[i].field->type->toString() : std::string("?")) + "'";
        }
        StructInfo* firstTarget = path[0].target->templateOf
            ? path[0].target->templateOf : path[0].target;
        std::string example = asciiOf(firstTarget->name);
        msg += ". Struct fields are stored inline, so this layout would be infinitely large. "
            "Break the cycle, for example by declaring '" + example + "' as a class "
            "('class " + example + " { ... }') so the field stores a reference.";
        const FieldInfo& f = *path[0].field;
        sink.error({f.line, f.column, static_cast<int>(f.name.size())}, std::move(msg));
    }
}

void Analyzer::checkFieldInitialization(const ast::StructDecl& sd) {
    // A struct field may have any type, including a non-defaultable one (a class,
    // string, array, or another non-defaultable struct). Such a struct is itself
    // non-defaultable, so it cannot be zero-initialized and must be built with an
    // aggregate literal or a constructor; the non-defaultable rules at each use
    // site (locals, array elements) still apply.
    (void)sd;
}

void Analyzer::checkFieldInitialization(const ast::ClassDecl& cd) {
    Type* t = analysis.typeOf(cd.node.greenNode());
    if (!t || !t->structInfo) return;

    // A class that declares any constructor is checked by the definite-assignment
    // pass over each constructor body. Only a class with no constructor at all is
    // caught here: it has no path on which a non-defaultable field could be assigned.
    for (auto& m : cd.methods()) {
        if (m.isConstructor()) return;
    }

    int baseFieldCount = t->structInfo->baseFieldCount;
    for (size_t fi = 0; fi < t->structInfo->fields.size(); ++fi) {
        if (static_cast<int>(fi) < baseFieldCount) continue;
        auto& f = t->structInfo->fields[fi];
        if (isDefaultable(f.type)) continue;
        if (fieldHasDefaultValue(f)) continue;

        std::string fname = asciiOf(f.name);
        std::string cname = asciiOf(t->structInfo->name);
        std::string msg = "Field '" + fname + "' of class '" + cname +
            "' has non-nullable type '" + f.type->toString() +
            "' but is never initialized. Give it a default (e.g. `" +
            f.type->toString() + " " + fname + " = ...;`), make it nullable (`" +
            f.type->toString() + "? " + fname + ";`), or add a constructor that assigns it " +
            "(via a `this." + fname + "` parameter or `this." + fname + " = ...;` in the body).";
        sink.error({f.line, f.column, static_cast<int>(fname.size())}, std::move(msg));
    }
}

bool Analyzer::validateArrayElement(Type* elem, const SyntaxNode& diagNode,
                                    const std::optional<std::u16string>& fillExampleName) {
    if (!elem || elem->isError()) return false;
    if (isDefaultable(elem)) return true;
    // A type-parameter backing store (`new T[n]`) is allowed: the slots start as
    // the substituted type's zero value and the generic container only reads the
    // ones it has filled.
    if (elem->isTypeParam()) return true;
    std::string hint;
    if (elem->isClass() || elem->isArray() || elem->isExternal() ||
        elem->kind == TypeKind::String) {
        if (fillExampleName) {
            std::string name = asciiOf(*fillExampleName);
            errorAtNode(diagNode, "An array of non-nullable '" + elem->toString() +
                "' must be filled as it is created: assign it to a new variable and "
                "follow the declaration with `for (long i = 0; i < " + name +
                ".length; i++) { " + name + "[i] = ...; }` (the update may be written "
                "'i++', '++i', or 'i = i + 1'). Alternatively use '" +
                elem->toString() + "?[]' so slots can start as null.");
            return false;
        }
        hint = " Use '" + elem->toString() + "?[]' so freshly-allocated slots can start as null.";
    } else if (elem->isStruct() && elem->structInfo) {
        const FieldInfo* bad = nullptr;
        for (auto& f : elem->structInfo->fields) {
            if (!isDefaultable(f.type)) { bad = &f; break; }
        }
        if (bad) {
            std::string fname;
            fname.reserve(bad->name.size());
            for (char16_t c : bad->name) fname.push_back(c < 128 ? static_cast<char>(c) : '?');
            hint = " Field '" + fname + "' of '" + elem->toString() +
                "' has type '" + (bad->type ? bad->type->toString() : std::string("?")) +
                "' which cannot start as a default zero value. Make that field nullable.";
        }
    }
    errorAtNode(diagNode, "Cannot use '" + elem->toString() +
        "' as an array element type because freshly-allocated slots would not be valid values of '" +
        elem->toString() + "'." + hint);
    return false;
}
