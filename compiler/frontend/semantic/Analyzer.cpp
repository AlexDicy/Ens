#include "Analyzer.h"

#include <algorithm>
#include "../diagnostics/Diagnostic.h"
#include "../diagnostics/DiagnosticSink.h"

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

Analyzer::Analyzer(const SourceFile& src, DiagnosticSink& s)
    : source(src), sink(s),
      ownedTypeCtx(std::make_unique<TypeContext>()),
      typeCtx(*ownedTypeCtx) {
    auto scope = std::make_unique<Scope>(nullptr);
    globalScope = scope.get();
    currentScope = globalScope;
    ownedScopes.push_back(std::move(scope));
    registerBuiltins();
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

// =========================================================
// Top-level pipeline
// =========================================================

void Analyzer::analyze(const SyntaxNode& root) {
    collectDeclarations(root);
    bindImports([](const std::u16string&) -> const Analyzer* { return nullptr; });
    analyzeBodies();
}

void Analyzer::collectDeclarations(const SyntaxNode& root) {
    auto sf = ast::SourceFile::cast(root);
    if (!sf) return;
    astRoot = sf;

    registerStructNames(*sf);
    registerClassNames(*sf);
    collectStructs(*sf);
    collectClasses(*sf);
    collectFunctions(*sf);
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
            // Named import: `import Alias from path;` — bring `Alias` into scope.
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
            // Namespace import: `import path;` — last path segment becomes the alias.
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

    for (auto& fn : sf.functions()) analyzeFunctionBody(fn);
    for (auto& sd : sf.structs())   for (auto& m : sd.methods()) analyzeFunctionBody(m);
    for (auto& cd : sf.classes())   for (auto& m : cd.methods()) analyzeFunctionBody(m);
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

void Analyzer::collectClasses(const ast::SourceFile& file) {
    auto classes = file.classes();

    for (auto& cd : classes) {
        Type* t = analysis.typeOf(cd.node.greenNode());
        if (!t) continue;
        for (auto& f : cd.fields()) {
            FieldInfo fi;
            auto fname = f.nameText();
            if (fname) fi.name = *fname;
            Type* ft = f.typeReference() ? resolveTypeReference(*f.typeReference()) : typeCtx.getError();
            fi.type = ft;
            fi.visibility = toSemanticVisibility(f.visibility());
            fi.isWeak = f.isWeak();
            if (fi.isWeak) {
                bool ok = ft && ft->isOptional() && ft->inner && ft->inner->isClass();
                if (!ok) {
                    errorAtNode(f.node,
                        "'weak' fields must be nullable class types (e.g. `weak Foo? f`)");
                }
            }
            auto [line, col] = source.offsetToPosition(f.node.startOffset());
            fi.line = line;
            fi.column = col;
            fi.declaration = f.node.greenNode();
            t->structInfo->fields.push_back(std::move(fi));
        }
    }

    for (auto& cd : classes) {
        Type* t = analysis.typeOf(cd.node.greenNode());
        if (!t) continue;
        for (auto& m : cd.methods()) {
            Type* retType = m.returnType() && m.returnType()->typeReference()
                ? resolveTypeReference(*m.returnType()->typeReference())
                : typeCtx.getPrimitive(TypeKind::Void);
            auto mname = m.nameText().value_or(std::u16string{});
            uint32_t mPos = m.nameToken() ? m.nameToken()->startOffset() : m.node.startOffset();
            Symbol* sym = makeSymbol(SymbolKind::Function, mname, nullptr, mPos);
            sym->returnType = retType;
            sym->funcDeclCst = m.node.greenNode();
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

    if (fn.isShorthand() && !isCtor) {
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

void Analyzer::checkFieldDefaults(const ast::StructDecl& sd) {
    Type* t = analysis.typeOf(sd.node.greenNode());
    if (!t || !t->structInfo) return;

    Symbol* prevFunction = currentFunction;
    Symbol* prevThis = currentThis;
    Scope* prevScope = currentScope;
    currentFunction = nullptr;
    currentThis = nullptr;
    currentScope = globalScope;

    auto fields = sd.fields();
    for (size_t i = 0; i < fields.size(); ++i) {
        auto& f = fields[i];
        auto dv = f.defaultValue();
        if (!dv) continue;
        auto dvExpr = dv->expression();
        if (!dvExpr) continue;
        Type* expected = (i < t->structInfo->fields.size()) ? t->structInfo->fields[i].type : typeCtx.getError();
        Type* actual = analyzeExpr(*dvExpr);
        if (!expected->isError() && !actual->isError() && !expected->assignableFrom(actual)) {
            errorAtNode(dvExpr->node, "Default value for field '" +
                asciiOf(f.nameText().value_or(std::u16string{})) + "': expected '" +
                expected->toString() + "', got '" + actual->toString() + "'");
        }
    }

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

    auto fields = cd.fields();
    for (size_t i = 0; i < fields.size(); ++i) {
        auto& f = fields[i];
        auto dv = f.defaultValue();
        if (!dv) continue;
        auto dvExpr = dv->expression();
        if (!dvExpr) continue;
        Type* expected = (i < t->structInfo->fields.size()) ? t->structInfo->fields[i].type : typeCtx.getError();
        Type* actual = analyzeExpr(*dvExpr);
        if (!expected->isError() && !actual->isError() && !expected->assignableFrom(actual)) {
            errorAtNode(dvExpr->node, "Default value for field '" +
                asciiOf(f.nameText().value_or(std::u16string{})) + "': expected '" +
                expected->toString() + "', got '" + actual->toString() + "'");
        }
    }

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
        Type* actual = analyzeExpr(*dvExpr);
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
            if (sym->type && (sym->type->isStruct() || sym->type->isClass())) return sym->type;
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
    if (tr.isOptional()) {
        if (base->isVoid()) {
            errorAtNode(tr.node, "void cannot be optional");
            result = typeCtx.getError();
        } else {
            result = typeCtx.getOptional(base);
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
    currentFunction = info->resolvedSymbol;

    pushScope();

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

    if (auto body = fn.body()) {
        for (auto& s : body->statements()) analyzeStatement(s);
    }

    popScope();
    currentFunction = prevFunction;
    currentThis = prevThis;
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
}

void Analyzer::analyzeBlock(const ast::Block& block) {
    pushScope();
    for (auto& s : block.statements()) analyzeStatement(s);
    popScope();
}

void Analyzer::analyzeLetStmt(const ast::LetStatement& stmt) {
    Type* declared = nullptr;
    if (auto tr = stmt.typeAnnotation()) {
        declared = resolveTypeReference(*tr);
        if (declared->isVoid()) {
            errorAtNode(tr->node, "Variable cannot have void type");
            declared = typeCtx.getError();
        }
    }

    Type* initType = nullptr;
    if (auto init = stmt.initializer()) initType = analyzeExpr(*init);

    auto name = stmt.nameText().value_or(std::u16string{});
    Type* finalType = declared;
    if (!declared && !initType) {
        errorAtNode(stmt.node, "Variable '" + asciiOf(name) + "' needs a type or an initializer");
        finalType = typeCtx.getError();
    } else if (!declared) {
        if (initType->isNull()) {
            errorAtNode(stmt.node, "Cannot infer type from 'null' alone - annotate the type, e.g. 'let " +
                asciiOf(name) + ": T? = null;'");
            finalType = typeCtx.getError();
        } else {
            finalType = initType;
        }
    } else if (initType && !declared->assignableFrom(initType)) {
        errorAtNode(stmt.node, "Cannot assign value of type '" + initType->toString() +
            "' to variable of type '" + declared->toString() + "'");
    }

    uint32_t namePos = stmt.nameToken() ? stmt.nameToken()->startOffset() : stmt.node.startOffset();
    Symbol* sym = makeSymbol(SymbolKind::Variable, name, finalType, namePos);
    if (!currentScope->define(sym)) {
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
    if (auto init = stmt.initializer()) initType = analyzeExpr(*init);
    if (initType && !declared->isError() && !declared->assignableFrom(initType)) {
        errorAtNode(stmt.node, "Cannot assign value of type '" + initType->toString() +
            "' to variable of type '" + declared->toString() + "'");
    }
    auto name = stmt.nameText().value_or(std::u16string{});
    uint32_t namePos = stmt.nameToken() ? stmt.nameToken()->startOffset() : stmt.node.startOffset();
    Symbol* sym = makeSymbol(SymbolKind::Variable, name, declared, namePos);
    if (!currentScope->define(sym)) {
        errorAtNode(stmt.node, "Variable '" + asciiOf(name) + "' is already defined in this scope");
    }
    analysis.setSymbol(stmt.node.greenNode(), sym);
}

Analyzer::NullCheckInfo Analyzer::detectNullCheck(const ast::Expression& cond) {
    NullCheckInfo info;
    auto bin = cond.asBinary();
    if (!bin) return info;
    auto opTok = bin->operatorToken();
    if (!opTok) return info;
    SyntaxKind op = opTok->kind();
    if (op != SyntaxKind::EqEq && op != SyntaxKind::NotEq) return info;
    auto left = bin->left();
    auto right = bin->right();
    if (!left || !right) return info;

    auto isNullLit = [](const ast::Expression& e) {
        auto lit = e.asLiteral();
        return lit && lit->literalKind() == SyntaxKind::KwNull;
    };

    const ast::Expression* identSide = nullptr;
    if (isNullLit(*right) && !isNullLit(*left)) identSide = &*left;
    else if (isNullLit(*left) && !isNullLit(*right)) identSide = &*right;
    else return info;

    auto id = identSide->asIdent();
    if (!id) return info;
    auto name = id->nameText();
    if (!name) return info;
    Symbol* sym = currentScope ? currentScope->lookup(*name) : nullptr;
    if (!sym || !sym->type || !sym->type->isOptional() || !sym->type->inner) return info;

    info.key = NarrowingPath{sym, {}};
    info.narrowedT = sym->type->inner;
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
    Type* actual = analyzeExpr(*value);
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
    else if (auto b  = expr.asBinary()) t = analyzeBinary(*b);
    else if (auto p  = expr.asPrefix()) t = analyzePrefix(*p);
    else if (auto c  = expr.asCall())   t = analyzeCall(*c);
    else if (auto m  = expr.asMember()) t = analyzeMember(*m);
    else if (auto sm = expr.asSafeMember()) t = analyzeSafeMember(*sm);
    else if (auto a  = expr.asAssign()) t = analyzeAssign(*a);
    else if (auto tn = expr.asTernary())t = analyzeTernary(*tn);
    else if (auto nw = expr.asNew())    t = analyzeNew(*nw);
    else if (auto pr = expr.asParen())  t = analyzeParen(*pr);
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

Type* Analyzer::analyzeBinary(const ast::BinaryExpression& expr) {
    auto left = expr.left();
    auto right = expr.right();
    if (!left || !right) return typeCtx.getError();
    Type* l = analyzeExpr(*left);
    Type* r = analyzeExpr(*right);
    if (l->isError() || r->isError()) return typeCtx.getError();

    auto opTok = expr.operatorToken();
    SyntaxKind op = opTok ? opTok->kind() : SyntaxKind::Invalid;

    switch (op) {
        case SyntaxKind::Plus:
        case SyntaxKind::Minus:
        case SyntaxKind::Star:
        case SyntaxKind::Slash:
        case SyntaxKind::Percent:
            if (!l->isNumeric() || !r->isNumeric()) {
                errorAtNode(expr.node, "Operator requires numeric operands, got '" +
                    l->toString() + "' and '" + r->toString() + "'");
                return typeCtx.getError();
            }
            if (!l->equals(r)) {
                errorAtNode(expr.node, "Operands must be the same type, got '" +
                    l->toString() + "' and '" + r->toString() + "'");
                return typeCtx.getError();
            }
            return l;

        case SyntaxKind::EqEq:
        case SyntaxKind::NotEq:
            if (!l->assignableFrom(r) && !r->assignableFrom(l)) {
                errorAtNode(expr.node, "Cannot compare '" + l->toString() + "' and '" + r->toString() + "'");
            }
            return typeCtx.getPrimitive(TypeKind::Bool);

        case SyntaxKind::Lt:
        case SyntaxKind::Gt:
        case SyntaxKind::LtEq:
        case SyntaxKind::GtEq:
            if (!l->isNumeric() || !r->isNumeric() || !l->equals(r)) {
                errorAtNode(expr.node, "Comparison requires matching numeric operands, got '" +
                    l->toString() + "' and '" + r->toString() + "'");
            }
            return typeCtx.getPrimitive(TypeKind::Bool);

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
                Type* argT = analyzeExpr(args[i]);
                Type* paramT = methodSym->paramTypes[i];
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
                Type* argT = analyzeExpr(args[i]);
                Type* paramT = methodSym->paramTypes[i];
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

    size_t req = requiredArgCount(sym);
    if (args.size() < req || args.size() > sym->paramTypes.size()) {
        errorAtNode(expr.node, "Function '" + asciiOf(*name) + "' expects " +
            std::to_string(req) +
            (req == sym->paramTypes.size() ? "" : "-" + std::to_string(sym->paramTypes.size())) +
            " argument(s), got " + std::to_string(args.size()));
    }
    size_t n = std::min(args.size(), sym->paramTypes.size());
    for (size_t i = 0; i < n; ++i) {
        Type* argT = analyzeExpr(args[i]);
        Type* paramT = sym->paramTypes[i];
        if (!paramT->assignableFrom(argT)) {
            errorAtNode(args[i].node, "Argument " + std::to_string(i + 1) +
                ": expected '" + paramT->toString() + "', got '" + argT->toString() + "'");
        }
    }
    for (size_t i = n; i < args.size(); ++i) analyzeExpr(args[i]);
    return sym->returnType ? sym->returnType : typeCtx.getError();
}

Type* Analyzer::analyzeMember(const ast::MemberExpression& expr) {
    auto obj = expr.object();
    if (!obj) return typeCtx.getError();

    // Namespace alias on the LHS: `ns.Name` — resolve `Name` against the
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
        return objT->structInfo->fields[idx].type;
    }
    int midx = objT->structInfo->findMethodIndex(*memberName);
    if (midx >= 0) {
        analysis.setMethodSymbol(expr.node.greenNode(), objT->structInfo->methods[midx].symbol);
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
        Type* fieldT = inner->structInfo->fields[idx].type;
        if (!isClassOrClassOptional(fieldT)) {
            errorAtNode(expr.node, "'?.' on '" + asciiOf(*memberName) +
                "' is not yet supported because the field has type '" + fieldT->toString() +
                "'. Only class-typed fields can be read through '?.' for now.");
            return typeCtx.getError();
        }
        return typeCtx.getOptional(fieldT);
    }
    int midx = inner->structInfo->findMethodIndex(*memberName);
    if (midx >= 0) {
        analysis.setMethodSymbol(expr.node.greenNode(), inner->structInfo->methods[midx].symbol);
        return typeCtx.getError();
    }
    errorAtNode(expr.node, "No field or method named '" + asciiOf(*memberName) +
        "' on '" + inner->toString() + "'.");
    return typeCtx.getError();
}

Type* Analyzer::analyzeAssign(const ast::AssignExpression& expr) {
    auto target = expr.target();
    auto value = expr.value();
    if (!target || !value) return typeCtx.getError();
    if (!isLValue(*target)) {
        errorAtNode(expr.node, "Left side of assignment must be an assignable expression");
    }
    Type* targetT = analyzeExpr(*target);
    Type* valueT = analyzeExpr(*value);

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

    if (!assignTargetT->isError() && !valueT->isError()) {
        if (!assignTargetT->assignableFrom(valueT)) {
            errorAtNode(expr.node, "Cannot assign '" + valueT->toString() +
                "' to '" + assignTargetT->toString() + "'");
        }
    }
    if (targetIdentSym && currentScope) {
        currentScope->clearNarrowingsContaining(targetIdentSym, u"");
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
    if (thenT->equals(elseT)) return thenT;
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
            Type* argT = analyzeExpr(args[i]);
            Type* paramT = ctor->paramTypes[i];
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

bool Analyzer::isLValue(const ast::Expression& expr) const {
    SyntaxKind k = expr.kind();
    return k == SyntaxKind::IdentExpr || k == SyntaxKind::MemberExpr ||
           k == SyntaxKind::SubscriptExpr || k == SyntaxKind::ThisExpr;
}
