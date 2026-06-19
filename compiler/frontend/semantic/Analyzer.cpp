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
    }
    return Visibility::Public;
}

static std::string asciiOf(std::u16string_view s) {
    std::string r;
    r.reserve(s.size());
    for (char16_t c : s) r.push_back(c < 128 ? static_cast<char>(c) : '?');
    return r;
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
      typeCtx(*ownedTypeCtx) {
    auto scope = std::make_unique<Scope>(nullptr);
    globalScope = scope.get();
    currentScope = globalScope;
    ownedScopes.push_back(std::move(scope));
    registerBuiltins();
    bootstrapPrelude();
}

Analyzer::Analyzer(const SourceFile& src, DiagnosticSink& s,
                   TypeContext& sharedContext, std::u16string mp)
    : source(src), sink(s),
      ownedTypeCtx(),
      typeCtx(sharedContext),
      modulePath_(std::move(mp)) {
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
}

Symbol* Analyzer::makeSymbol(SymbolKind k, std::u16string n, Type* t, uint32_t offset) {
    auto [line, column] = source.offsetToPosition(offset);
    auto s = std::make_unique<Symbol>(k, std::move(n), t, line, column);
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

namespace {

const ast::LiteralExpression* asIntLiteralChild(const ast::Expression& e) {
    if (auto lit = e.asLiteral()) {
        static thread_local std::optional<ast::LiteralExpression> hold;
        if (lit->literalKind() != SyntaxKind::IntLiteral) return nullptr;
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
            if (lit->literalKind() != SyntaxKind::IntLiteral) return nullptr;
            static thread_local std::optional<ast::LiteralExpression> hold;
            hold = lit;
            return &*hold;
        }
    }
    return nullptr;
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
    analyzeBodies();
    if (astRoot) {
        ThrowsAnalyzer throwsAnalyzer(*astRoot, analysis, errorClassInfo_);
        throwsAnalyzer.analyze();
        throwsAnalyzer.validate(sink, source);
    }
}

void Analyzer::collectDeclarations(const SyntaxNode& root) {
    registerNames(root);
    resolveSignatures();
    if (astRoot) layoutDeclaredClasses(*astRoot);
}

void Analyzer::registerNames(const SyntaxNode& root) {
    auto sf = ast::SourceFile::cast(root);
    if (!sf) return;
    astRoot = sf;

    registerStructNames(*sf);
    registerClassNames(*sf);
    registerExternalTypeNames(*sf);
}

void Analyzer::resolveSignatures() {
    if (!astRoot) return;
    auto& sf = *astRoot;

    collectStructs(sf);
    resolveClassBases(sf);
    collectFunctions(sf);
    collectExternalFunctions(sf);
}

void Analyzer::registerExternalTypeNames(const ast::SourceFile& file) {
    for (auto& ed : file.externalTypes()) {
        auto name = ed.nameText();
        if (!name) continue;
        if (typeCtx.lookupNamedType(modulePath_, *name)) {
            errorAtNode(ed.node, "Duplicate type '" + asciiOf(*name) + "'");
            continue;
        }
        Type* t = typeCtx.registerExternalType(modulePath_, *name);
        auto [line, col] = source.offsetToPosition(ed.node.startOffset());
        if (t->structInfo) {
            t->structInfo->line = line;
            t->structInfo->column = col;
        }
        analysis.setType(ed.node.greenNode(), t);
    }
}

void Analyzer::collectExternalFunctions(const ast::SourceFile& file) {
    for (auto& block : file.externalBlocks()) {
        auto libName = block.libraryName();
        if (!libName || libName->empty()) {
            errorAtNode(block.node, "The library name in 'external from \"\"' cannot be empty.");
        } else {
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
                errorAtNode(decl.node, "Duplicate function name '" + asciiOf(fname) + "'");
            }
            analysis.setSymbol(decl.node.greenNode(), sym);
        }
    }
}

void Analyzer::registerStructNames(const ast::SourceFile& file) {
    for (auto& sd : file.structs()) {
        auto name = sd.nameText();
        if (!name) continue;
        if (typeCtx.lookupNamedType(modulePath_, *name)) {
            errorAtNode(sd.node, "Duplicate type '" + asciiOf(*name) + "'");
            continue;
        }
        Type* t = typeCtx.registerStruct(modulePath_, *name);
        auto [line, col] = source.offsetToPosition(sd.node.startOffset());
        t->structInfo->line = line;
        t->structInfo->column = col;
        analysis.setType(sd.node.greenNode(), t);
    }
}

void Analyzer::registerClassNames(const ast::SourceFile& file) {
    for (auto& cd : file.classes()) {
        auto name = cd.nameText();
        if (!name) continue;
        if (typeCtx.lookupNamedType(modulePath_, *name)) {
            errorAtNode(cd.node, "Duplicate type '" + asciiOf(*name) + "'");
            continue;
        }
        Type* t = typeCtx.registerClass(modulePath_, *name);
        auto [line, col] = source.offsetToPosition(cd.node.startOffset());
        t->structInfo->line = line;
        t->structInfo->column = col;
        analysis.setType(cd.node.greenNode(), t);
    }
}

void Analyzer::bindImports(const ModuleResolver& resolver) {
    if (!astRoot) return;
    for (auto& imp : astRoot->imports()) {
        if (imp.isPackage()) {
            errorAtNode(imp.node, "Package imports are not yet supported");
            continue;
        }
        std::u16string targetPath = imp.modulePath();
        const Analyzer* target = resolver(targetPath);
        if (!target) {
            errorAtNode(imp.node, "Cannot resolve import '" + asciiOf(targetPath) + "'");
            continue;
        }

        if (auto alias = imp.aliasText()) {
            // Named import: `import Alias from path;`, bring `Alias` into scope.
            Type* importedType = typeCtx.lookupNamedType(targetPath, *alias);
            uint32_t namePos = imp.aliasToken() ? imp.aliasToken()->startOffset() : imp.node.startOffset();
            if (importedType) {
                Symbol* sym = makeSymbol(SymbolKind::Variable, *alias, importedType, namePos);
                if (!globalScope->define(sym)) {
                    errorAtNode(imp.node, "Imported name '" + asciiOf(*alias) +
                        "' conflicts with an existing declaration");
                }
                continue;
            }
            Symbol* fnSym = target->globalSymbol(*alias);
            if (fnSym && fnSym->kind == SymbolKind::Function) {
                if (!globalScope->define(fnSym)) {
                    errorAtNode(imp.node, "Imported name '" + asciiOf(*alias) +
                        "' conflicts with an existing declaration");
                }
                continue;
            }
            errorAtNode(imp.node, "Module '" + asciiOf(targetPath) +
                "' has no exported '" + asciiOf(*alias) + "'");
        } else {
            // Namespace import: `import path;`, last path segment becomes the alias.
            auto nsName = imp.namespaceName();
            if (!nsName) continue;
            Symbol* sym = makeSymbol(SymbolKind::Namespace, *nsName, nullptr, imp.node.startOffset());
            sym->namespaceModulePath = targetPath;
            if (!globalScope->define(sym)) {
                errorAtNode(imp.node, "Namespace alias '" + asciiOf(*nsName) +
                    "' conflicts with an existing declaration");
            }
        }
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

    for (auto& fn : sf.functions()) analyzeFunctionBody(fn);
    for (auto& sd : sf.structs())   for (auto& m : sd.methods()) analyzeFunctionBody(m);
    for (auto& cd : sf.classes())   for (auto& m : cd.methods()) analyzeFunctionBody(m);
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

void Analyzer::collectStructs(const ast::SourceFile& file) {
    auto structs = file.structs();

    for (auto& sd : structs) {
        Type* t = analysis.typeOf(sd.node.greenNode());
        if (!t) continue;
        for (auto& f : sd.fields()) {
            FieldInfo fi;
            auto fname = f.nameText();
            if (fname) fi.name = *fname;
            Type* ft = f.typeReference() ? resolveTypeReference(*f.typeReference()) : typeCtx.getError();
            fi.type = ft;
            fi.visibility = toSemanticVisibility(f.visibility());
            fi.isWeak = f.isWeak();
            if (fi.isWeak) {
                errorAtNode(f.node, "'weak' fields are not allowed on structs");
            }
            auto [line, col] = source.offsetToPosition(f.node.startOffset());
            fi.line = line;
            fi.column = col;
            fi.declaration = f.node.greenNode();
            t->structInfo->fields.push_back(std::move(fi));
        }
    }

    for (auto& sd : structs) {
        Type* t = analysis.typeOf(sd.node.greenNode());
        if (!t) continue;
        for (auto& m : sd.methods()) {
            Type* retType = m.returnType() && m.returnType()->typeReference()
                ? resolveTypeReference(*m.returnType()->typeReference())
                : typeCtx.getPrimitive(TypeKind::Void);
            auto mname = m.nameText().value_or(std::u16string{});
            uint32_t mPos = m.nameToken() ? m.nameToken()->startOffset() : m.node.startOffset();
            Symbol* sym = makeSymbol(SymbolKind::Function, mname, nullptr, mPos);
            sym->returnType = retType;
            sym->funcDeclCst = m.node.greenNode();
            sym->declaredThrows = m.isThrows();
            sym->abiThrows = m.isThrows();  // structs have no inheritance
            sym->methodOwner = t->structInfo;
            bool isCtor = (mname == t->structInfo->name);
            checkFieldMethodCollision(t->structInfo, mname, isCtor, m.node);
            checkThrowsClausePlacement(m, /*isOverridable=*/false, /*isConstructor=*/isCtor);
            resolveMethodParams(m, t, sym);
            analysis.setSymbol(m.node.greenNode(), sym);
            analysis.setReceiver(m.node.greenNode(), t);

            MethodInfo mi;
            mi.name = mname;
            mi.symbol = sym;
            mi.declaration = const_cast<GreenElement*>(m.node.greenNode());
            mi.visibility = toSemanticVisibility(m.visibility());
            t->structInfo->methods.push_back(std::move(mi));
        }
    }
}

static int baseDepth(StructInfo* si) {
    int d = 0;
    for (StructInfo* s = si->baseInfo; s; s = s->baseInfo) ++d;
    return d;
}

void Analyzer::resolveClassBases(const ast::SourceFile& file) {
    auto classes = file.classes();

    // --- Pass A: record class modifiers and resolve base links. ---
    for (auto& cd : classes) {
        Type* t = analysis.typeOf(cd.node.greenNode());
        if (!t || !t->structInfo) continue;
        StructInfo* si = t->structInfo;
        si->isAbstract = cd.isAbstract();
        si->isFinal = cd.isFinal();
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
            } else if (!baseT->isClass() || !baseT->structInfo) {
                errorAtNode(diag, "'" + asciiOf(*baseName) +
                    "' is not a class; only classes can be extended");
            } else if (baseT->structInfo == si) {
                errorAtNode(diag, "Class '" + asciiOf(si->name) + "' cannot extend itself");
            } else if (baseT->structInfo->isFinal) {
                errorAtNode(diag, "Cannot extend '" + asciiOf(*baseName) +
                    "' because it is declared 'final'");
            } else {
                si->baseInfo = baseT->structInfo;
            }
        }
    }

    // --- Detect inheritance cycles; break the offending link. ---
    for (auto& cd : classes) {
        Type* t = analysis.typeOf(cd.node.greenNode());
        if (!t || !t->structInfo) continue;
        StructInfo* si = t->structInfo;
        std::unordered_set<StructInfo*> seen;
        for (StructInfo* s = si; s; s = s->baseInfo) {
            if (!seen.insert(s).second) {
                errorAtNode(cd.node, "Class '" + asciiOf(si->name) +
                    "' eventually extends itself through its base classes. "
                    "A class cannot inherit from itself, directly or indirectly.");
                si->baseInfo = nullptr;
                break;
            }
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

void Analyzer::layoutOneClass(const ast::ClassDecl& cd) {
    Type* t = analysis.typeOf(cd.node.greenNode());
    if (!t || !t->structInfo) return;
    StructInfo* si = t->structInfo;

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
        fi.visibility = toSemanticVisibility(f.visibility());
        fi.isWeak = f.isWeak();
        fi.definingClass = si;
        if (fi.isWeak) {
            bool ok = ft && ft->isOptional() && ft->inner && ft->inner->isClass();
            if (!ok) {
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
        auto [line, col] = source.offsetToPosition(f.node.startOffset());
        fi.line = line;
        fi.column = col;
        fi.declaration = f.node.greenNode();
        si->fields.push_back(std::move(fi));
    }

    // Methods: collect own methods, then validate override/abstract.
    for (auto& m : cd.methods()) {
        Type* retType = m.returnType() && m.returnType()->typeReference()
            ? resolveTypeReference(*m.returnType()->typeReference())
            : typeCtx.getPrimitive(TypeKind::Void);
        auto mname = m.nameText().value_or(std::u16string{});
        uint32_t mPos = m.nameToken() ? m.nameToken()->startOffset() : m.node.startOffset();
        Symbol* sym = makeSymbol(SymbolKind::Function, mname, nullptr, mPos);
        sym->returnType = retType;
        sym->funcDeclCst = m.node.greenNode();
        sym->declaredThrows = m.isThrows();
        sym->methodOwner = si;
        resolveMethodParams(m, t, sym);
        analysis.setSymbol(m.node.greenNode(), sym);
        analysis.setReceiver(m.node.greenNode(), t);

        MethodInfo mi;
        mi.name = mname;
        mi.symbol = sym;
        mi.declaration = const_cast<GreenElement*>(m.node.greenNode());
        mi.visibility = toSemanticVisibility(m.visibility());
        mi.isOverride = m.isOverride();
        mi.isFinal = m.isFinal();
        mi.isAbstract = m.isAbstract();
        mi.definingClass = si;
        si->methods.push_back(std::move(mi));

        // Validate (base methods already collected via base-before-derived order).
        bool isCtor = (mname == si->name);
        bool overridable = !isCtor && !m.isFinal() && !si->isFinal;
        checkFieldMethodCollision(si, mname, isCtor, m.node);
        checkThrowsClausePlacement(m, overridable, isCtor);
        if (isCtor) {
            if (m.isOverride() || m.isAbstract())
                errorAtNode(m.node, "A constructor cannot be 'override' or 'abstract'");
            continue;
        }
        StructInfo* baseDecl = si->baseInfo ? si->baseInfo->classDeclaringMethod(mname) : nullptr;
        if (m.isAbstract()) {
            if (!si->isAbstract)
                errorAtNode(m.node, "Abstract method '" + asciiOf(mname) + "' requires class '" +
                    asciiOf(si->name) + "' to be declared 'abstract'");
            if (m.body().has_value())
                errorAtNode(m.node, "Abstract method '" + asciiOf(mname) + "' cannot have a body");
        }
        if (m.isOverride()) {
            if (!baseDecl) {
                errorAtNode(m.node, "Method '" + asciiOf(mname) +
                    "' is marked 'override' but no base class declares it");
            } else {
                MethodInfo& bm = baseDecl->methods[baseDecl->findMethodIndex(mname)];
                if (bm.isFinal)
                    errorAtNode(m.node, "Cannot override '" + asciiOf(mname) +
                        "' because it is declared 'final' in '" + asciiOf(baseDecl->name) + "'");
                if (!overrideSignaturesCompatible(bm, sym))
                    errorAtNode(m.node, "Override of '" + asciiOf(mname) +
                        "' does not match the signature declared in '" + asciiOf(baseDecl->name) + "'");
                if (m.isThrows() && bm.symbol && !bm.symbol->declaredThrows)
                    errorAtNode(m.throwsToken().value_or(m.node), "Method '" + asciiOf(mname) +
                        "' is marked 'throws' but overrides a method of '" + asciiOf(baseDecl->name) +
                        "' that is not. Mark the base method 'throws' too, or handle the exceptions "
                        "inside the override.");
            }
        } else if (baseDecl) {
            errorAtNode(m.node, "Method '" + asciiOf(mname) + "' hides a method inherited from '" +
                asciiOf(baseDecl->name) + "'; mark it 'override' to replace it, or rename it");
        }
    }

    // --- A concrete class must implement every inherited abstract method. ---
    if (!si->isAbstract) {
        std::unordered_set<std::u16string> checked;
        for (StructInfo* s = si; s; s = s->baseInfo) {
            for (auto& m : s->methods) {
                if (m.name == s->name) continue;  // constructor
                if (!checked.insert(m.name).second) continue;
                StructInfo* decl = si->classDeclaringMethod(m.name);  // most-derived declaration
                if (decl && decl->methods[decl->findMethodIndex(m.name)].isAbstract) {
                    errorAtNode(cd.node, "Class '" + asciiOf(si->name) +
                        "' must override abstract method '" + asciiOf(m.name) +
                        "', or be declared 'abstract'");
                }
            }
        }
    }
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
}

void Analyzer::finalizeClassHierarchy(const std::vector<StructInfo*>& classes) {
    std::vector<StructInfo*> order(classes.begin(), classes.end());
    std::stable_sort(order.begin(), order.end(),
        [](StructInfo* a, StructInfo* b) { return baseDepth(a) < baseDepth(b); });

    // --- Assign vtable slots: a method is virtual only when abstract or overridden somewhere. ---
    for (StructInfo* si : order) {  // phase 1: mark
        for (auto& mi : si->methods) {
            if (mi.name == si->name) continue;
            if (mi.isAbstract) mi.vtableSlot = VTSLOT_PENDING;
            if (mi.isOverride && si->baseInfo) {
                if (StructInfo* bc = si->baseInfo->classDeclaringMethod(mi.name)) {
                    mi.vtableSlot = VTSLOT_PENDING;
                    bc->methods[bc->findMethodIndex(mi.name)].vtableSlot = VTSLOT_PENDING;
                }
            }
        }
    }
    for (StructInfo* si : order) {  // phase 2: assign indices
        si->vtableSize = si->baseInfo ? si->baseInfo->vtableSize : 0;
        for (auto& mi : si->methods) {
            if (mi.name == si->name || mi.vtableSlot != VTSLOT_PENDING) continue;
            StructInfo* bc = si->baseInfo ? si->baseInfo->classDeclaringMethod(mi.name) : nullptr;
            int inherited = bc ? bc->methods[bc->findMethodIndex(mi.name)].vtableSlot : -1;
            mi.vtableSlot = (inherited >= 0) ? inherited : si->vtableSize++;
        }
    }

    // ABI throws-ness is uniform across a vtable slot
    for (StructInfo* si : order) {
        for (auto& mi : si->methods) {
            if (mi.name == si->name || !mi.symbol) continue;
            StructInfo* root = si->rootClassDeclaringMethod(mi.name);
            int ri = root ? root->findMethodIndex(mi.name) : -1;
            Symbol* rootSym = (ri >= 0) ? root->methods[ri].symbol : nullptr;
            mi.symbol->abiThrows = rootSym ? rootSym->declaredThrows : mi.symbol->declaredThrows;
        }
    }
}

void Analyzer::collectFunctions(const ast::SourceFile& file) {
    for (auto& fn : file.functions()) {
        Type* retType = fn.returnType() && fn.returnType()->typeReference()
            ? resolveTypeReference(*fn.returnType()->typeReference())
            : typeCtx.getPrimitive(TypeKind::Void);
        auto fname = fn.nameText().value_or(std::u16string{});
        uint32_t fPos = fn.nameToken() ? fn.nameToken()->startOffset() : fn.node.startOffset();
        Symbol* sym = makeSymbol(SymbolKind::Function, fname, nullptr, fPos);
        sym->returnType = retType;
        sym->funcDeclCst = fn.node.greenNode();
        sym->declaredThrows = fn.isThrows();
        sym->abiThrows = fn.isThrows();
        checkThrowsClausePlacement(fn, /*isOverridable=*/false, /*isConstructor=*/false);
        resolveFunctionParams(fn, sym);
        if (!globalScope->define(sym)) {
            errorAtNode(fn.node, "Duplicate function name '" + asciiOf(fname) + "'");
        }
        analysis.setSymbol(fn.node.greenNode(), sym);
    }
}

void Analyzer::resolveMethodParams(const ast::FuncDecl& fn, ::Type* receiverType, Symbol* sym) {
    auto fname = fn.nameText().value_or(std::u16string{});
    bool isCtor = receiverType && receiverType->structInfo && fname == receiverType->structInfo->name;

    if (fn.isShorthand() && !isCtor && !fn.isAbstract()) {
        errorAtNode(fn.node, "Shorthand declaration ';' is only allowed on a constructor");
    }

    bool seenDefault = false;
    for (auto& p : fn.parameters()) {
        Type* pt = nullptr;
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
            uint32_t fOffset = f.node.startOffset();
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
            uint32_t fOffset = f.node.startOffset();
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

Type* Analyzer::lookupTypeByName(const std::u16string& qualifier,
                                 const std::u16string& name,
                                 const SyntaxNode& diagNode) {
    if (qualifier.empty()) {
        if (Type* prim = typeCtx.primitiveFromName(name)) return prim;
        if (Type* t = typeCtx.lookupNamedType(modulePath_, name)) return t;
        // Fall back to imported aliases stored in the module's globalScope.
        if (Symbol* sym = globalScope ? globalScope->lookupLocal(name) : nullptr) {
            if (sym->type && (sym->type->isStruct() || sym->type->isClass() || sym->type->isExternal())) return sym->type;
        }
        errorAtNode(diagNode, "Unknown type '" + asciiOf(name) + "'");
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

    Scope* funcScope = pushScope();
    currentFunctionParamScope = funcScope;

    Type* receiverType = analysis.receiverOf(fn.node.greenNode());

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
        if (!currentScope->define(psym)) {
            errorAtNode(p.node, "Duplicate parameter name '" + asciiOf(pname) + "'");
        }
        analysis.setSymbol(p.node.greenNode(), psym);
    }

    // Synthesize this.field = paramName for each this-field param.
    if (receiverType) analyzeImplicitConstructorAssignments(fn);

    // Body locals live in a child scope so catch clauses (siblings below) can't see them.
    pushScope();
    if (auto body = fn.body()) {
        for (auto& s : body->statements()) analyzeStatement(s);
    }
    popScope();

    // A constructor must chain to its base when the base has no zero-argument constructor.
    if (receiverType && receiverType->structInfo && !sawSuperConstructorCall) {
        StructInfo* cls = receiverType->structInfo;
        bool isCtor = currentFunction && currentFunction->name == cls->name;
        if (isCtor && cls->baseInfo) {
            int bidx = cls->baseInfo->findMethodIndex(cls->baseInfo->name);
            if (bidx >= 0) {
                Symbol* baseCtor = cls->baseInfo->methods[bidx].symbol;
                if (baseCtor && requiredArgCount(baseCtor) > 0) {
                    errorAtNode(fn.node, "Constructor of '" + asciiOf(cls->name) +
                        "' must call 'super(...)' because base class '" +
                        asciiOf(cls->baseInfo->name) + "' has no zero-argument constructor");
                }
            }
        }
    }

    for (auto& cc : fn.catchClauses()) analyzeCatchClause(cc, funcScope);

    popScope();
    currentFunction = prevFunction;
    currentThis = prevThis;
    sawSuperConstructorCall = prevSawSuper;
    currentFunctionParamScope = prevParamScope;
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

    if (auto nameTok = clause.nameToken()) {
        auto cname = clause.nameText().value_or(std::u16string{});
        Symbol* var = makeSymbol(SymbolKind::Variable, cname, clauseType, nameTok->startOffset());
        currentScope->define(var);
        analysis.setSymbol(clause.node.greenNode(), var);
    }

    bool prevInCatch = inCatchClause;
    inCatchClause = true;
    if (auto body = clause.body()) {
        for (auto& s : body->statements()) analyzeStatement(s);
    }
    inCatchClause = prevInCatch;

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

void Analyzer::analyzeStatement(const ast::Statement& stmt) {
    if (auto b = stmt.asBlock())              { analyzeBlock(*b); return; }
    if (auto l = stmt.asLet())                { analyzeLetStmt(*l); return; }
    if (auto v = stmt.asTypedVarDecl())       { analyzeTypedVarDeclStmt(*v); return; }
    if (auto i = stmt.asIf())                 { analyzeIfStmt(*i); return; }
    if (auto w = stmt.asWhile())              { analyzeWhileStmt(*w); return; }
    if (auto r = stmt.asReturn())             { analyzeReturnStmt(*r); return; }
    if (auto e = stmt.asExpressionStmt())     { analyzeExpressionStmt(*e); return; }
    if (auto th = stmt.asThrow())             { analyzeThrowStmt(*th); return; }
    if (auto rt = stmt.asRethrow())           { analyzeRethrowStmt(*rt); return; }
}

void Analyzer::analyzeBlock(const ast::Block& block) {
    pushScope();
    for (auto& s : block.statements()) analyzeStatement(s);
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
    bool collides = !currentScope->define(sym);
    if (!collides && currentScope->parent == currentFunctionParamScope &&
        currentFunctionParamScope && currentFunctionParamScope->lookupLocal(name)) {
        collides = true;
    }
    if (collides) {
        errorAtNode(stmt.node, "Variable '" + asciiOf(name) + "' is already defined in this scope");
    }
    analysis.setSymbol(stmt.node.greenNode(), sym);
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
    if (!initType && !isDefaultable(declared) && !declared->isError()) {
        errorAtNode(stmt.node, "Variable '" + asciiOf(name) + "' has non-nullable type '" +
            declared->toString() + "' but no initializer. Provide a value, or make the type nullable ('" +
            declared->toString() + "?').");
    }
    uint32_t namePos = stmt.nameToken() ? stmt.nameToken()->startOffset() : stmt.node.startOffset();
    Symbol* sym = makeSymbol(SymbolKind::Variable, name, declared, namePos);
    bool collides = !currentScope->define(sym);
    if (!collides && currentScope->parent == currentFunctionParamScope &&
        currentFunctionParamScope && currentFunctionParamScope->lookupLocal(name)) {
        collides = true;
    }
    if (collides) {
        errorAtNode(stmt.node, "Variable '" + asciiOf(name) + "' is already defined in this scope");
    }
    analysis.setSymbol(stmt.node.greenNode(), sym);
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
    std::vector<Symbol*>* indexSymbols) const {
    ast::Expression core = unwrapParens(expr);

    if (auto id = core.asIdent()) {
        auto* info = analysis.find(id->node.greenNode());
        Symbol* sym = info ? info->resolvedSymbol : nullptr;
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
        auto base = buildNarrowingPath(*obj, indexSymbols);
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
        auto base = buildNarrowingPath(*obj, indexSymbols);
        if (!base) return std::nullopt;
        ast::Expression idxCore = unwrapParens(*idx);
        PathSegment seg;
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
                if (!tok) return std::nullopt;
                uint64_t magnitude = 0;
                if (!parseIntegerLiteralMagnitude(std::u16string(tok->tokenText()), magnitude)) {
                    return std::nullopt;
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
            auto* info = analysis.find(idId->node.greenNode());
            Symbol* sym = info ? info->resolvedSymbol : nullptr;
            if (!sym) return std::nullopt;
            if (sym->kind != SymbolKind::Variable && sym->kind != SymbolKind::Parameter) {
                return std::nullopt;
            }
            seg.kind = PathSegment::Kind::IdentIndex;
            seg.identIndexSym = sym;
            base->chain.push_back(std::move(seg));
            if (indexSymbols) indexSymbols->push_back(sym);
            return base;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

void Analyzer::clearNarrowingsForCall(const ast::CallExpression& expr) {
    if (!currentScope) return;
    auto dropRoot = [&](const ast::Expression& e) {
        if (auto p = buildNarrowingPath(e)) {
            currentScope->clearNarrowingsForRoot(p->root);
        }
    };
    // Receiver (method / safe-method call): the call could mutate state
    // reachable through it.
    if (auto callee = expr.callee()) {
        if (auto m = callee->asMember()) {
            if (auto obj = m->object()) dropRoot(*obj);
        } else if (auto sm = callee->asSafeMember()) {
            if (auto obj = sm->object()) dropRoot(*obj);
        }
    }
    // Arguments: only class / array references expose mutable state; struct
    // and primitive args are passed by value, so the call can't mutate
    // through them.
    for (const auto& arg : expr.arguments()) {
        if (arg.asOutArgument()) continue;
        Type* t = analysis.typeOf(arg.node.greenNode());
        if (!t) continue;
        Type* base = t->isOptional() ? t->inner : t;
        if (!base) continue;
        if (!base->isClass() && !base->isArray()) continue;
        dropRoot(arg);
    }
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

void Analyzer::analyzeBranchWithNarrowing(const ast::Block& block,
                                          const NullCheckInfo& info, bool installNarrowing) {
    pushScope();
    if (installNarrowing && info.valid) {
        currentScope->narrowedTypes[info.key] = info.narrowedT;
    }
    for (auto& s : block.statements()) analyzeStatement(s);
    popScope();
}

void Analyzer::analyzeIfStmt(const ast::IfStatement& stmt) {
    NullCheckInfo info;
    if (auto c = stmt.condition()) {
        Type* ct = analyzeExpr(*c);
        if (!ct->isError() && !ct->isBool()) {
            errorAtNode(c->node, "If condition must be 'bool', got '" + ct->toString() + "'");
        }
        info = detectNullCheck(*c);
    }
    if (auto b = stmt.thenBlock()) {
        analyzeBranchWithNarrowing(*b, info, info.valid && info.narrowsThen);
    }
    if (auto ec = stmt.elseClause()) {
        if (auto inner = ec->ifStatement()) analyzeIfStmt(*inner);
        else if (auto bb = ec->block()) {
            analyzeBranchWithNarrowing(*bb, info, info.valid && !info.narrowsThen);
        }
    }
}

void Analyzer::analyzeWhileStmt(const ast::WhileStatement& stmt) {
    if (auto c = stmt.condition()) {
        Type* ct = analyzeExpr(*c);
        if (!ct->isError() && !ct->isBool()) {
            errorAtNode(c->node, "While condition must be 'bool', got '" + ct->toString() + "'");
        }
    }
    if (auto b = stmt.body()) analyzeBlock(*b);
}

void Analyzer::analyzeReturnStmt(const ast::ReturnStatement& stmt) {
    if (!currentFunction) {
        errorAtNode(stmt.node, "'return' outside of a function");
        return;
    }
    Type* expected = currentFunction->returnType;
    auto value = stmt.value();
    if (!value) {
        if (expected && !expected->isVoid()) {
            errorAtNode(stmt.node, "Function returns '" + expected->toString() +
                "', but 'return' has no value");
        }
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
}

void Analyzer::analyzeExpressionStmt(const ast::ExpressionStatement& stmt) {
    if (auto e = stmt.expression()) analyzeExpr(*e);
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
    else if (auto c  = expr.asCall())   t = analyzeCall(*c);
    else if (auto m  = expr.asMember()) t = analyzeMember(*m);
    else if (auto sm = expr.asSafeMember()) t = analyzeSafeMember(*sm);
    else if (auto su = expr.asSubscript()) t = analyzeSubscript(*su);
    else if (auto ss = expr.asSafeSubscript()) t = analyzeSafeSubscript(*ss);
    else if (auto ca = expr.asCast()) t = analyzeCast(*ca);
    else if (auto oa = expr.asOutArgument()) {
        errorAtNode(expr.node, "'out' can only be used when calling an external function.");
        t = typeCtx.getError();
    }
    else if (auto a  = expr.asAssign()) t = analyzeAssign(*a);
    else if (auto tn = expr.asTernary())t = analyzeTernary(*tn);
    else if (auto nw = expr.asNew())    t = analyzeNew(*nw);
    else if (auto tr = expr.asTry())    t = analyzeTry(*tr);
    else if (auto pr = expr.asParen())  t = analyzeParen(*pr);
    else if (auto al = expr.asArrayLiteral()) t = analyzeArrayLiteral(*al);
    else                                t = typeCtx.getError();
    analysis.setType(expr.node.greenNode(), t);
    return t;
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
    Type* t = typeCtx.lookupClass(modulePath_, definingClass->name);
    return t && t->structInfo == definingClass;
}

bool Analyzer::isMemberAccessAllowed(Visibility visibility, StructInfo* definingClass) {
    if (visibility == Visibility::Public) return true;
    StructInfo* current = (currentThis && currentThis->type) ? currentThis->type->structInfo : nullptr;
    if (visibility == Visibility::Private) {
        return current && current == definingClass;
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
    } else {
        errorAtNode(diagNode, "'" + asciiOf(memberName) + "' is protected to class '" + owner +
            "' and can only be accessed from inside '" + owner +
            "', a subclass of it, or the file where '" + owner + "' is declared");
    }
}

Type* Analyzer::analyzeBinary(const ast::BinaryExpression& expr) {
    auto left = expr.left();
    auto right = expr.right();
    if (!left || !right) return typeCtx.getError();
    Type* l = analyzeExpr(*left);
    Type* r = analyzeExpr(*right);
    if (l->isError() || r->isError()) return typeCtx.getError();

    auto opTok = expr.operatorToken();
    SyntaxKind op = opTok ? opTok->kind() : SyntaxKind::Invalid;

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
            if (!l->isNumeric() || !r->isNumeric()) {
                errorAtNode(expr.node, "Operator requires numeric operands, got '" +
                    l->toString() + "' and '" + r->toString() + "'");
                return typeCtx.getError();
            }
            tryAdaptOperands();
            Type* common = numericCommonType(l, r);
            if (!common) {
                errorAtNode(expr.node, "Operands must be the same type, got '" +
                    l->toString() + "' and '" + r->toString() + "'");
                return typeCtx.getError();
            }
            return common;
        }

        case SyntaxKind::EqEq:
        case SyntaxKind::NotEq:
            tryAdaptOperands();
            if (!l->assignableFrom(r) && !r->assignableFrom(l)) {
                errorAtNode(expr.node, "Cannot compare '" + l->toString() + "' and '" + r->toString() + "'");
            }
            return typeCtx.getPrimitive(TypeKind::Bool);

        case SyntaxKind::Lt:
        case SyntaxKind::Gt:
        case SyntaxKind::LtEq:
        case SyntaxKind::GtEq: {
            if (!l->isNumeric() || !r->isNumeric()) {
                errorAtNode(expr.node, "Comparison requires numeric operands, got '" +
                    l->toString() + "' and '" + r->toString() + "'");
                return typeCtx.getPrimitive(TypeKind::Bool);
            }
            tryAdaptOperands();
            if (!numericCommonType(l, r)) {
                errorAtNode(expr.node, "Comparison requires matching numeric operands, got '" +
                    l->toString() + "' and '" + r->toString() + "'");
            }
            return typeCtx.getPrimitive(TypeKind::Bool);
        }

        case SyntaxKind::AmpAmp:
        case SyntaxKind::PipePipe:
            if (!l->isBool() || !r->isBool()) {
                errorAtNode(expr.node, "Logical operator requires bool operands, got '" +
                    l->toString() + "' and '" + r->toString() + "'");
            }
            return typeCtx.getPrimitive(TypeKind::Bool);

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
            if (!t->isNumeric()) {
                errorAtNode(expr.node, "Increment/decrement requires numeric, got '" + t->toString() + "'");
                return typeCtx.getError();
            }
            if (!isLValue(*operand)) {
                errorAtNode(expr.node, "Cannot increment/decrement a non-assignable expression");
            }
            return t;
        default:
            errorAtNode(expr.node, "Unsupported unary operator");
            return typeCtx.getError();
    }
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

Type* Analyzer::analyzeCall(const ast::CallExpression& expr) {
    Type* result = [&]() -> Type* {
    auto callee = expr.callee();
    auto args = expr.arguments();

    // Method call: obj.method(args)
    if (callee && callee->asMember()) {
        auto member = *callee->asMember();
        analyzeExpr(*callee);  // resolves field-or-method on member
        auto* memberInfo = analysis.find(member.node.greenNode());
        Symbol* methodSym = memberInfo ? memberInfo->resolvedMethodSymbol : nullptr;
        if (methodSym) {
            size_t req = requiredArgCount(methodSym);
            if (args.size() < req || args.size() > methodSym->paramTypes.size()) {
                auto mname = member.memberText().value_or(std::u16string{});
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
        analyzeExpr(*callee);  // resolves field-or-method on safe-member
        auto* memberInfo = analysis.find(member.node.greenNode());
        Symbol* methodSym = memberInfo ? memberInfo->resolvedMethodSymbol : nullptr;
        if (methodSym) {
            auto mname = member.memberText().value_or(std::u16string{});
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
            if (ret->isVoid()) {
                errorAtNode(expr.node, "Cannot use '?.' to call '" + asciiOf(mname) +
                    "' because it does not return a value.");
                return typeCtx.getError();
            }
            bool retIsClassish = ret->isClass() ||
                (ret->isOptional() && ret->inner && ret->inner->isClass());
            if (!retIsClassish) {
                errorAtNode(expr.node, "'?.' on '" + asciiOf(mname) +
                    "' is not yet supported because it returns '" + ret->toString() +
                    "'. Only methods that return a class type can be called through '?.' for now.");
                return typeCtx.getError();
            }
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
        bool inCtor = cls && currentFunction && currentFunction->name == cls->name;
        if (!inCtor) {
            errorAtNode(expr.node, "'super(...)' can only be called from a constructor");
        }
        if (!base) {
            for (auto& a : args) analyzeExpr(a);
            return typeCtx.getPrimitive(TypeKind::Void);
        }
        int cidx = base->findMethodIndex(base->name);
        if (cidx < 0) {
            if (!args.empty())
                errorAtNode(expr.node, "Base class '" + asciiOf(base->name) +
                    "' has no constructor, so 'super(...)' takes no arguments");
            for (auto& a : args) analyzeExpr(a);
            return typeCtx.getPrimitive(TypeKind::Void);
        }
        Symbol* ctorSym = base->methods[cidx].symbol;
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

    if (sym->isExternal) {
        return analyzeExternalCall(expr, sym, *name);
    }

    size_t req = requiredArgCount(sym);
    if (args.size() < req || args.size() > sym->paramTypes.size()) {
        errorAtNode(expr.node, "Function '" + asciiOf(*name) + "' expects " +
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
    }();
    clearNarrowingsForCall(expr);
    return result;
}

Type* Analyzer::analyzeExternalCall(const ast::CallExpression& expr, Symbol* sym,
                                    const std::u16string& funcName) {
    auto args = expr.arguments();
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
                    analysis.setType(expr.node.greenNode(), t);
                    return t;
                }
                errorAtNode(expr.node, "Module '" + asciiOf(nsSym->namespaceModulePath) +
                    "' has no '" + asciiOf(*memberName) + "'");
                return typeCtx.getError();
            }
        }
    }

    Type* objT = analyzeExpr(*obj);
    if (objT->isError()) return typeCtx.getError();
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
        if (fieldT && fieldT->isOptional() && currentScope) {
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
        checkMemberAccess(expr.node, *memberName, mi.visibility, mi.definingClass);
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

    if (!objT->isOptional()) {
        errorAtNode(expr.node, "The value on the left of '?.' has type '" + objT->toString() +
            "', which can never be null. Use '.' to access its members.");
        return typeCtx.getError();
    }
    Type* inner = objT->inner;
    if (!inner || !inner->hasRecordLayout() || !inner->structInfo) {
        std::string innerName = inner ? inner->toString() : std::string("?");
        errorAtNode(expr.node, "The value on the left of '?.' has type '" + innerName +
            "?', which has no members to access.");
        return typeCtx.getError();
    }
    if (!inner->isClass()) {
        errorAtNode(expr.node, "'?.' is not yet supported on '" + inner->toString() +
            "?'. Only nullable class types can use '?.' for now.");
        return typeCtx.getError();
    }

    auto memberName = expr.memberText();
    if (!memberName) return typeCtx.getError();

    auto isClassOrClassOptional = [](Type* t) {
        if (!t) return false;
        if (t->isClass()) return true;
        if (t->isOptional() && t->inner && t->inner->isClass()) return true;
        return false;
    };

    int idx = inner->structInfo->findFieldIndex(*memberName);
    if (idx >= 0) {
        const FieldInfo& fld = inner->structInfo->fields[idx];
        checkMemberAccess(expr.node, *memberName, fld.visibility, fld.definingClass);
        Type* fieldT = fld.type;
        if (!isClassOrClassOptional(fieldT)) {
            errorAtNode(expr.node, "'?.' on '" + asciiOf(*memberName) +
                "' is not yet supported because the field has type '" + fieldT->toString() +
                "'. Only class-typed fields can be read through '?.' for now.");
            return typeCtx.getError();
        }
        return typeCtx.getOptional(fieldT);
    }
    if (StructInfo* decl = inner->structInfo->classDeclaringMethod(*memberName)) {
        const MethodInfo& mi = decl->methods[decl->findMethodIndex(*memberName)];
        checkMemberAccess(expr.node, *memberName, mi.visibility, mi.definingClass);
        analysis.setMethodSymbol(expr.node.greenNode(), mi.symbol);
        return typeCtx.getError();
    }
    errorAtNode(expr.node, "No field or method named '" + asciiOf(*memberName) +
        "' on '" + inner->toString() + "'.");
    return typeCtx.getError();
}

Type* Analyzer::analyzeCast(const ast::CastExpression& expr) {
    auto src = expr.source();
    auto tr = expr.targetType();
    if (!src || !tr) return typeCtx.getError();
    Type* srcT = analyzeExpr(*src);
    Type* dstT = resolveTypeReference(*tr);
    if (srcT->isError() || dstT->isError()) return typeCtx.getError();

    auto isNumeric = [](Type* t) { return t && (t->isInteger() || t->isFloat()); };
    if (!isNumeric(srcT) || !isNumeric(dstT)) {
        errorAtNode(expr.node, "Cannot cast '" + srcT->toString() + "' to '" +
            dstT->toString() + "'; 'as' only supports numeric conversions.");
        return typeCtx.getError();
    }
    return dstT;
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
    if (elemT && elemT->isOptional() && currentScope) {
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
    // Element type must be a reference type (class, array, or optional of those)
    // so that the result `T?` is representable. Primitive optionals are not
    // currently supported in codegen.
    auto isReferenceish = [](Type* t) {
        if (!t) return false;
        if (t->isClass() || t->isArray()) return true;
        if (t->isOptional() && t->inner &&
            (t->inner->isClass() || t->inner->isArray())) return true;
        return false;
    };
    if (!isReferenceish(elem)) {
        errorAtNode(expr.node, "'?[' on '" + objT->toString() +
            "' is not supported because its elements have type '" + elem->toString() +
            "'. Only arrays of class or array elements can be indexed through '?['.");
        return typeCtx.getError();
    }
    if (!idxT->isError() && !idxT->isInteger()) {
        errorAtNode(idx->node, "Array index must be an integer, got '" + idxT->toString() + "'");
    }
    return typeCtx.getOptional(elem);
}

Type* Analyzer::analyzeAssign(const ast::AssignExpression& expr) {
    auto target = expr.target();
    auto value = expr.value();
    if (!target || !value) return typeCtx.getError();
    if (!isLValue(*target)) {
        errorAtNode(expr.node, "Left side of assignment must be an assignable expression");
    }
    Type* targetT = analyzeExpr(*target);

    // For a narrowed identifier the storage keeps its declared (wider) type; the
    // narrowing only governs reads. Use the symbol's declared type when checking
    // assignability so that e.g. `x = null` still works inside `if x != null { }`.
    Type* assignTargetT = targetT;
    Symbol* targetIdentSym = nullptr;
    if (auto id = target->asIdent()) {
        if (auto* targetInfo = analysis.find(id->node.greenNode())) {
            if (Symbol* sym = targetInfo->resolvedSymbol) {
                targetIdentSym = sym;
                if (sym->type) assignTargetT = sym->type;
            }
        }
    }

    Type* valueT = analyzeExprAdapt(*value, assignTargetT);

    if (!assignTargetT->isError() && !valueT->isError()) {
        if (!assignTargetT->assignableFrom(valueT)) {
            errorAtNode(expr.node, "Cannot assign '" + valueT->toString() +
                "' to '" + assignTargetT->toString() + "'");
        }
    }
    if (targetIdentSym && currentScope) {
        currentScope->clearNarrowingsForRoot(targetIdentSym);
        currentScope->clearNarrowingsForIndexSymbol(targetIdentSym);
    } else if (currentScope && (target->asMember() || target->asSubscript())) {
        if (auto p = buildNarrowingPath(*target)) {
            currentScope->clearNarrowingsAtOrBelow(*p);
        }
    }
    return targetT;
}

Type* Analyzer::analyzeTernary(const ast::TernaryExpression& expr) {
    auto cond = expr.condition();
    auto thenE = expr.thenBranch();
    auto elseE = expr.elseBranch();
    Type* condT = cond ? analyzeExpr(*cond) : typeCtx.getError();
    Type* thenT = thenE ? analyzeExpr(*thenE) : typeCtx.getError();
    Type* elseT = elseE ? analyzeExpr(*elseE) : typeCtx.getError();
    if (cond && !condT->isError() && !condT->isBool()) {
        errorAtNode(cond->node, "Ternary condition must be 'bool', got '" + condT->toString() + "'");
    }
    if (thenT->isError() || elseT->isError()) return typeCtx.getError();
    // Adapt polymorphic int literal in one branch toward the other branch's type.
    if (thenE) tryAdaptIntegerLiteral(*thenE, elseT);
    if (elseE) tryAdaptIntegerLiteral(*elseE, thenT);
    if (thenE) { Type* upd = analysis.typeOf(thenE->node.greenNode()); if (upd) thenT = upd; }
    if (elseE) { Type* upd = analysis.typeOf(elseE->node.greenNode()); if (upd) elseT = upd; }
    if (thenT->equals(elseT)) return thenT;
    if (Type* common = numericCommonType(thenT, elseT)) return common;
    if (thenT->assignableFrom(elseT)) return thenT;
    if (elseT->assignableFrom(thenT)) return elseT;
    errorAtNode(expr.node, "Ternary branches have incompatible types '" + thenT->toString() +
        "' and '" + elseT->toString() + "'");
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
        // element-nullability rule. When `unsized > 0`, the slots at the
        // deepest allocated level hold nullable inner arrays (which are
        // defaultable as `null`), so T itself is never zero-initialized and
        // doesn't need to be defaultable here.
        Type* slotElem;
        if (unsized == 0) {
            if (!validateArrayElement(elem, tr->node)) {
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

    if (t->structInfo && t->structInfo->isAbstract) {
        errorAtNode(expr.node, "Cannot create an instance of abstract class '" +
            asciiOf(*typeName) + "'; instantiate a concrete subclass instead");
    }

    Symbol* ctor = nullptr;
    int ctorIdx = t->structInfo->findMethodIndex(t->structInfo->name);
    if (ctorIdx >= 0) ctor = t->structInfo->methods[ctorIdx].symbol;

    auto args = expr.arguments();
    if (ctor) {
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
    if (!value) return;
    Type* t = analyzeExpr(*value);
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

bool Analyzer::isLValue(const ast::Expression& expr) const {
    SyntaxKind k = expr.kind();
    return k == SyntaxKind::IdentExpr || k == SyntaxKind::MemberExpr ||
           k == SyntaxKind::SubscriptExpr || k == SyntaxKind::ThisExpr;
}

static bool fieldHasDefaultValue(const FieldInfo& f) {
    if (!f.declaration) return false;
    auto fieldNode = SyntaxNode::makeRoot(f.declaration);
    auto fd = ast::FieldDecl::cast(*fieldNode);
    return fd && fd->defaultValue().has_value();
}

bool Analyzer::isDefaultable(Type* t) const {
    if (!t) return false;
    if (t->isPrimitive()) return true;
    if (t->isOptional()) return true;
    if (t->isStruct()) {
        if (!t->structInfo) return false;
        for (auto& f : t->structInfo->fields) {
            if (isDefaultable(f.type)) continue;
            if (fieldHasDefaultValue(f)) continue;
            return false;
        }
        return true;
    }
    return false;
}

static bool ctorHasThisFieldParam(const ast::FuncDecl& ctor, const std::u16string& fieldName) {
    for (auto& p : ctor.parameters()) {
        if (!p.isThisField()) continue;
        if (auto pname = p.nameText(); pname && *pname == fieldName) return true;
    }
    return false;
}

// Returns true if there is at least one top-level assignment in the body.
// Branches and loops aren't credited as not considered to guarantee initialization.
static bool ctorBodyAssignsThisField(const ast::FuncDecl& ctor,
                                     const std::u16string& fieldName) {
    auto body = ctor.body();
    if (!body) return false;
    for (auto& s : body->statements()) {
        auto e = s.asExpressionStmt();
        if (!e) continue;
        auto expr = e->expression();
        if (!expr) continue;
        auto a = expr->asAssign();
        if (!a) continue;
        auto target = a->target();
        if (!target) continue;
        auto m = target->asMember();
        if (!m) continue;
        auto obj = m->object();
        if (!obj || !obj->asThis()) continue;
        auto mname = m->memberText();
        if (mname && *mname == fieldName) return true;
    }
    return false;
}

void Analyzer::checkFieldInitialization(const ast::StructDecl& sd) {
    Type* t = analysis.typeOf(sd.node.greenNode());
    if (!t || !t->structInfo) return;
    for (auto& f : t->structInfo->fields) {
        if (isDefaultable(f.type)) continue;
        if (fieldHasDefaultValue(f)) continue;
        std::string fname = asciiOf(f.name);
        std::string sname = asciiOf(t->structInfo->name);
        std::string msg = "Field '" + fname + "' of struct '" + sname +
            "' has non-nullable type '" + f.type->toString() +
            "' and no default value. Either give it a default (e.g. `" +
            f.type->toString() + " " + fname + " = ...;`) or make it nullable (`" +
            f.type->toString() + "? " + fname + ";`).";
        sink.error({f.line, f.column, static_cast<int>(fname.size())}, std::move(msg));
    }
}

void Analyzer::checkFieldInitialization(const ast::ClassDecl& cd) {
    Type* t = analysis.typeOf(cd.node.greenNode());
    if (!t || !t->structInfo) return;
    std::u16string className = t->structInfo->name;

    std::vector<ast::FuncDecl> ctors;
    for (auto& m : cd.methods()) {
        if (auto mname = m.nameText(); mname && *mname == className) ctors.push_back(m);
    }

    int baseFieldCount = t->structInfo->baseFieldCount;
    for (size_t fi = 0; fi < t->structInfo->fields.size(); ++fi) {
        if (static_cast<int>(fi) < baseFieldCount) continue;
        auto& f = t->structInfo->fields[fi];
        if (isDefaultable(f.type)) continue;
        if (fieldHasDefaultValue(f)) continue;

        bool everyCtorInits = !ctors.empty();
        for (auto& ctor : ctors) {
            if (!ctorHasThisFieldParam(ctor, f.name) &&
                !ctorBodyAssignsThisField(ctor, f.name)) {
                everyCtorInits = false;
                break;
            }
        }
        if (everyCtorInits) continue;

        std::string fname = asciiOf(f.name);
        std::string cname = asciiOf(t->structInfo->name);
        std::string msg = "Field '" + fname + "' of class '" + cname +
            "' has non-nullable type '" + f.type->toString() +
            "' but is never initialized. Give it a default (e.g. `" +
            f.type->toString() + " " + fname + " = ...;`), make it nullable (`" +
            f.type->toString() + "? " + fname + ";`), or initialize it in every constructor " +
            "(via `this." + fname + "` parameter or `this." + fname + " = ...;` in the body).";
        sink.error({f.line, f.column, static_cast<int>(fname.size())}, std::move(msg));
    }
}

bool Analyzer::validateArrayElement(Type* elem, const SyntaxNode& diagNode) {
    if (!elem || elem->isError()) return false;
    if (isDefaultable(elem)) return true;
    std::string hint;
    if (elem->isClass() || elem->isArray() || elem->isExternal() ||
        elem->kind == TypeKind::String) {
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
