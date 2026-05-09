#include "Analyzer.h"
#include "../tokenizer/TokenType.h"
#include <string>

static std::string asciiOf(std::u16string_view s) {
    std::string r;
    r.reserve(s.size());
    for (char16_t c : s) r.push_back(c < 128 ? static_cast<char>(c) : '?');
    return r;
}

Analyzer::Analyzer() {
    auto scope = std::make_unique<Scope>(nullptr);
    globalScope = scope.get();
    currentScope = globalScope;
    ownedScopes.push_back(std::move(scope));

    registerBuiltins();
}

void Analyzer::registerBuiltins() {
    // print(string) -> void  (maps to C `puts` in codegen)
    Type* voidTy   = typeCtx.getPrimitive(TypeKind::Void);
    Type* stringTy = typeCtx.getPrimitive(TypeKind::String);

    Symbol* printSym = makeSymbol(SymbolKind::Function, std::u16string(u"print"), nullptr, 0, 0);
    printSym->returnType = voidTy;
    printSym->paramTypes = {stringTy};
    printSym->isBuiltin = true;
    globalScope->define(printSym);
}

Symbol* Analyzer::makeSymbol(SymbolKind k, std::u16string n, Type* t, int line, int col) {
    auto s = std::make_unique<Symbol>(k, std::move(n), t, line, col);
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
    if (currentScope && currentScope->parent) {
        currentScope = currentScope->parent;
    }
}

void Analyzer::error(int line, int col, int len, std::string msg) {
    SourceSpan span{line, col, len > 0 ? len : 1};
    diagnostics.emplace_back(DiagnosticLevel::Error, span, std::move(msg));
}

void Analyzer::analyze(const std::vector<StmtPtr>& program) {
    collectStructs(program);
    collectClasses(program);
    collectFunctions(program);

    auto synthesizeAndCheck = [&](FuncDecl* fn) {
        synthesizeShorthandBody(fn);
        checkParameterDefaults(fn);
    };
    for (const auto& s : program) {
        if (auto* fn = dynamic_cast<FuncDecl*>(s.get())) synthesizeAndCheck(fn);
        else if (auto* sd = dynamic_cast<StructDecl*>(s.get())) for (auto& m : sd->methods) synthesizeAndCheck(m.get());
        else if (auto* cd = dynamic_cast<ClassDecl*>(s.get())) for (auto& m : cd->methods) synthesizeAndCheck(m.get());
    }

    for (const auto& s : program) {
        if (auto* fn = dynamic_cast<FuncDecl*>(s.get())) {
            analyzeFunctionBody(fn);
        } else if (auto* sd = dynamic_cast<StructDecl*>(s.get())) {
            for (auto& m : sd->methods) analyzeFunctionBody(m.get());
        } else if (auto* cd = dynamic_cast<ClassDecl*>(s.get())) {
            for (auto& m : cd->methods) analyzeFunctionBody(m.get());
        } else {
            analyzeStmt(s.get());
        }
    }
}

void Analyzer::collectStructs(const std::vector<StmtPtr>& program) {
    // First pass: register the names so structs can reference each other.
    for (const auto& s : program) {
        auto* sd = dynamic_cast<StructDecl*>(s.get());
        if (!sd) continue;
        if (typeCtx.lookupNamedType(sd->name)) {
            error(sd->line, sd->column, static_cast<int>(sd->name.size()),
                  "Duplicate type '" + asciiOf(sd->name) + "'");
            continue;
        }
        sd->resolvedType = typeCtx.registerStruct(sd->name);
        sd->resolvedType->structInfo->line = sd->line;
        sd->resolvedType->structInfo->column = sd->column;
    }
    // Second pass: resolve field types now that all struct names exist.
    for (const auto& s : program) {
        auto* sd = dynamic_cast<StructDecl*>(s.get());
        if (!sd || !sd->resolvedType) continue;
        for (auto& f : sd->fields) {
            Type* ft = resolveTypeNode(f.type.get());
            FieldInfo fi;
            fi.name = f.name;
            fi.type = ft;
            fi.line = f.line;
            fi.column = f.column;
            sd->resolvedType->structInfo->fields.push_back(std::move(fi));
        }
    }
    // Third pass: register method symbols on each struct so cross-method calls
    // (and forward references) resolve regardless of declaration order.
    for (const auto& s : program) {
        auto* sd = dynamic_cast<StructDecl*>(s.get());
        if (!sd || !sd->resolvedType) continue;
        for (auto& m : sd->methods) {
            Type* retType = m->returnType ? resolveTypeNode(m->returnType.get())
                                          : typeCtx.getPrimitive(TypeKind::Void);
            Symbol* sym = makeSymbol(SymbolKind::Function, m->name, nullptr,
                                     m->line, m->column);
            sym->returnType = retType;
            sym->funcDecl = m.get();
            m->resolvedSymbol = sym;
            m->receiverType = sd->resolvedType;
            resolveMethodParams(m.get(), sd->resolvedType, sym);

            MethodInfo mi;
            mi.name = m->name;
            mi.symbol = sym;
            mi.declaration = m.get();
            sd->resolvedType->structInfo->methods.push_back(std::move(mi));
        }
    }
}

void Analyzer::collectClasses(const std::vector<StmtPtr>& program) {
    // Pass 1: register names so classes can reference each other (and structs).
    for (const auto& s : program) {
        auto* cd = dynamic_cast<ClassDecl*>(s.get());
        if (!cd) continue;
        if (typeCtx.lookupNamedType(cd->name)) {
            error(cd->line, cd->column, static_cast<int>(cd->name.size()),
                  "Duplicate type '" + asciiOf(cd->name) + "'");
            continue;
        }
        cd->resolvedType = typeCtx.registerClass(cd->name);
        cd->resolvedType->structInfo->line = cd->line;
        cd->resolvedType->structInfo->column = cd->column;
    }
    // Pass 2: resolve field types.
    for (const auto& s : program) {
        auto* cd = dynamic_cast<ClassDecl*>(s.get());
        if (!cd || !cd->resolvedType) continue;
        for (auto& f : cd->fields) {
            Type* ft = resolveTypeNode(f.type.get());
            FieldInfo fi;
            fi.name = f.name;
            fi.type = ft;
            fi.line = f.line;
            fi.column = f.column;
            cd->resolvedType->structInfo->fields.push_back(std::move(fi));
        }
    }
    // Pass 3: register method symbols.
    for (const auto& s : program) {
        auto* cd = dynamic_cast<ClassDecl*>(s.get());
        if (!cd || !cd->resolvedType) continue;
        for (auto& m : cd->methods) {
            Type* retType = m->returnType ? resolveTypeNode(m->returnType.get())
                                          : typeCtx.getPrimitive(TypeKind::Void);
            Symbol* sym = makeSymbol(SymbolKind::Function, m->name, nullptr,
                                     m->line, m->column);
            sym->returnType = retType;
            sym->funcDecl = m.get();
            m->resolvedSymbol = sym;
            m->receiverType = cd->resolvedType;
            resolveMethodParams(m.get(), cd->resolvedType, sym);

            MethodInfo mi;
            mi.name = m->name;
            mi.symbol = sym;
            mi.declaration = m.get();
            cd->resolvedType->structInfo->methods.push_back(std::move(mi));
        }
    }
}

void Analyzer::collectFunctions(const std::vector<StmtPtr>& program) {
    for (const auto& s : program) {
        auto* fn = dynamic_cast<FuncDecl*>(s.get());
        if (!fn) continue;

        Type* retType = fn->returnType ? resolveTypeNode(fn->returnType.get())
                                       : typeCtx.getPrimitive(TypeKind::Void);

        Symbol* sym = makeSymbol(SymbolKind::Function, fn->name, nullptr, fn->line, fn->column);
        sym->returnType = retType;
        sym->funcDecl = fn;
        fn->resolvedSymbol = sym;
        resolveFunctionParams(fn, sym);
        if (!globalScope->define(sym)) {
            error(fn->line, fn->column, static_cast<int>(fn->name.size()),
                  "Duplicate function name '" + asciiOf(fn->name) + "'");
        }
    }
}

void Analyzer::resolveMethodParams(FuncDecl* fn, ::Type* receiverType, Symbol* sym) {
    bool isCtor = (receiverType->structInfo && fn->name == receiverType->structInfo->name);

    if (fn->isShorthand && !isCtor) {
        error(fn->line, fn->column, static_cast<int>(fn->name.size()),
              "Shorthand declaration ';' is only allowed on a constructor");
    }

    bool seenDefault = false;
    for (auto& p : fn->parameters) {
        Type* pt = nullptr;
        if (p.isThisField) {
            if (!isCtor) {
                error(fn->line, fn->column, static_cast<int>(p.name.size()),
                      "'this." + asciiOf(p.thisFieldName) + "' parameters are only allowed in a constructor");
                pt = typeCtx.getError();
            } else {
                int idx = receiverType->structInfo->findFieldIndex(p.thisFieldName);
                if (idx < 0) {
                    error(fn->line, fn->column, static_cast<int>(p.thisFieldName.size()),
                          "No field '" + asciiOf(p.thisFieldName) + "' on type '" + receiverType->toString() + "'");
                    pt = typeCtx.getError();
                } else {
                    pt = receiverType->structInfo->fields[idx].type;
                }
            }
        } else {
            pt = resolveTypeNode(p.type.get());
        }
        sym->paramTypes.push_back(pt);

        if (p.defaultValue) {
            seenDefault = true;
        } else if (seenDefault) {
            int line = p.type ? p.type->line : fn->line;
            int col  = p.type ? p.type->column : fn->column;
            error(line, col, static_cast<int>(p.name.size()),
                  "Parameter '" + asciiOf(p.name) + "' has no default but follows a defaulted parameter");
        }
    }
}

void Analyzer::resolveFunctionParams(FuncDecl* fn, Symbol* sym) {
    if (fn->isShorthand) {
        error(fn->line, fn->column, static_cast<int>(fn->name.size()),
              "Shorthand declaration ';' is only allowed on a constructor");
    }
    bool seenDefault = false;
    for (auto& p : fn->parameters) {
        if (p.isThisField) {
            error(fn->line, fn->column, static_cast<int>(p.name.size()),
                  "'this." + asciiOf(p.thisFieldName) + "' parameters are only allowed in a constructor");
            sym->paramTypes.push_back(typeCtx.getError());
        } else {
            sym->paramTypes.push_back(resolveTypeNode(p.type.get()));
        }
        if (p.defaultValue) {
            seenDefault = true;
        } else if (seenDefault) {
            int line = p.type ? p.type->line : fn->line;
            int col  = p.type ? p.type->column : fn->column;
            error(line, col, static_cast<int>(p.name.size()),
                  "Parameter '" + asciiOf(p.name) + "' has no default but follows a defaulted parameter");
        }
    }
}

void Analyzer::synthesizeShorthandBody(FuncDecl* fn) {
    bool hasThisField = false;
    for (auto& p : fn->parameters) if (p.isThisField) { hasThisField = true; break; }
    if (!fn->isShorthand && !hasThisField) return;

    std::vector<StmtPtr> synth;
    for (auto& p : fn->parameters) {
        if (!p.isThisField) continue;
        auto thisE = std::make_unique<ThisExpr>();
        thisE->line = fn->line;
        thisE->column = fn->column;
        auto memberE = std::make_unique<MemberExpr>(std::move(thisE), p.thisFieldName);
        memberE->line = fn->line;
        memberE->column = fn->column;
        auto identE = std::make_unique<IdentExpr>(p.name);
        identE->line = fn->line;
        identE->column = fn->column;
        auto assignE = std::make_unique<AssignExpr>(TokenType::EQ, std::move(memberE), std::move(identE));
        assignE->line = fn->line;
        assignE->column = fn->column;
        auto stmt = std::make_unique<ExprStmt>(std::move(assignE));
        stmt->line = fn->line;
        stmt->column = fn->column;
        synth.push_back(std::move(stmt));
    }

    if (fn->isShorthand) {
        auto block = std::make_unique<BlockStmt>();
        block->line = fn->line;
        block->column = fn->column;
        for (auto& s : synth) block->statements.push_back(std::move(s));
        fn->body = std::move(block);
    } else if (!synth.empty()) {
        std::vector<StmtPtr> combined;
        combined.reserve(synth.size() + fn->body->statements.size());
        for (auto& s : synth) combined.push_back(std::move(s));
        for (auto& s : fn->body->statements) combined.push_back(std::move(s));
        fn->body->statements = std::move(combined);
    }
}

void Analyzer::checkParameterDefaults(FuncDecl* fn) {
    Symbol* prevFunction = currentFunction;
    Symbol* prevThis = currentThis;
    Scope* prevScope = currentScope;
    currentFunction = nullptr;
    currentThis = nullptr;
    currentScope = globalScope;

    Symbol* sym = fn->resolvedSymbol;
    for (size_t i = 0; i < fn->parameters.size(); ++i) {
        auto& p = fn->parameters[i];
        if (!p.defaultValue) continue;
        Type* expected = (sym && i < sym->paramTypes.size()) ? sym->paramTypes[i] : typeCtx.getError();
        Type* actual = analyzeExpr(p.defaultValue.get());
        if (!expected->isError() && !actual->isError() && !expected->assignableFrom(actual)) {
            error(p.defaultValue->line, p.defaultValue->column, 1,
                  "Default value for parameter '" + asciiOf(p.name) + "': expected '" +
                  expected->toString() + "', got '" + actual->toString() + "'");
        }
    }

    currentFunction = prevFunction;
    currentThis = prevThis;
    currentScope = prevScope;
}

void Analyzer::analyzeFunctionBody(FuncDecl* fn) {
    Symbol* prevFunction = currentFunction;
    Symbol* prevThis = currentThis;
    currentFunction = fn->resolvedSymbol;

    pushScope();

    // Methods get an implicit `this` symbol bound to the receiver type.
    if (fn->receiverType) {
        Symbol* thisSym = makeSymbol(SymbolKind::Parameter, std::u16string(u"this"),
                                     fn->receiverType, fn->line, fn->column);
        currentScope->define(thisSym);
        currentThis = thisSym;
        fn->thisSymbol = thisSym;
    } else {
        currentThis = nullptr;
    }

    for (size_t i = 0; i < fn->parameters.size(); ++i) {
        auto& p = fn->parameters[i];
        Type* pt = (i < currentFunction->paramTypes.size()) ? currentFunction->paramTypes[i]
                                                            : typeCtx.getError();
        Symbol* sym = makeSymbol(SymbolKind::Parameter, p.name, pt, p.type ? p.type->line : fn->line, p.type ? p.type->column : fn->column);
        if (!currentScope->define(sym)) {
            error(p.type ? p.type->line : fn->line,
                  p.type ? p.type->column : fn->column,
                  static_cast<int>(p.name.size()),
                  "Duplicate parameter name '" + asciiOf(p.name) + "'");
        }
        p.resolvedSymbol = sym;
    }

    if (fn->body) {
        for (auto& s : fn->body->statements) {
            analyzeStmt(s.get());
        }
    }

    popScope();
    currentFunction = prevFunction;
    currentThis = prevThis;
}

Type* Analyzer::resolveTypeNode(TypeNode* node) {
    if (!node) return typeCtx.getError();
    Type* base = typeCtx.fromName(node->name);
    if (!base) {
        error(node->line, node->column, static_cast<int>(node->name.size()),
              "Unknown type '" + asciiOf(node->name) + "'");
        return typeCtx.getError();
    }
    if (node->isOptional) {
        if (base->isVoid()) {
            error(node->line, node->column, static_cast<int>(node->name.size()) + 1,
                  "void cannot be optional");
            return typeCtx.getError();
        }
        return typeCtx.getOptional(base);
    }
    return base;
}

void Analyzer::analyzeStmt(Stmt* s) {
    if (auto* b = dynamic_cast<BlockStmt*>(s))    { analyzeBlock(b); return; }
    if (auto* v = dynamic_cast<VarDeclStmt*>(s))  { analyzeVarDecl(v); return; }
    if (auto* i = dynamic_cast<IfStmt*>(s))       { analyzeIf(i); return; }
    if (auto* w = dynamic_cast<WhileStmt*>(s))    { analyzeWhile(w); return; }
    if (auto* r = dynamic_cast<ReturnStmt*>(s))   { analyzeReturn(r); return; }
    if (auto* e = dynamic_cast<ExprStmt*>(s))     { analyzeExprStmt(e); return; }
    if (dynamic_cast<FuncDecl*>(s)) {
        // Nested function declarations not supported yet
        error(s->line, s->column, 1, "Nested function declarations are not supported");
        return;
    }
    if (dynamic_cast<StructDecl*>(s)) {
        error(s->line, s->column, 1, "Nested struct declarations are not supported");
        return;
    }
    if (dynamic_cast<ClassDecl*>(s)) {
        error(s->line, s->column, 1, "Nested class declarations are not supported");
        return;
    }
}

void Analyzer::analyzeBlock(BlockStmt* s) {
    pushScope();
    for (auto& child : s->statements) {
        analyzeStmt(child.get());
    }
    popScope();
}

void Analyzer::analyzeVarDecl(VarDeclStmt* s) {
    Type* declared = nullptr;
    if (s->type) {
        declared = resolveTypeNode(s->type.get());
        if (declared->isVoid()) {
            error(s->type->line, s->type->column, 4, "Variable cannot have void type");
            declared = typeCtx.getError();
        }
    }

    Type* initType = nullptr;
    if (s->init) {
        initType = analyzeExpr(s->init.get());
    }

    Type* finalType = declared;
    if (!declared && !initType) {
        error(s->line, s->column, static_cast<int>(s->name.size()),
              "Variable '" + asciiOf(s->name) + "' needs a type or an initializer");
        finalType = typeCtx.getError();
    } else if (!declared) {
        // Pure inference. `null` alone gives no useful type.
        if (initType->isNull()) {
            error(s->line, s->column, static_cast<int>(s->name.size()),
                  "Cannot infer type from 'null' alone — annotate the type, e.g. 'let " + asciiOf(s->name) + ": T? = null;'");
            finalType = typeCtx.getError();
        } else {
            finalType = initType;
        }
    } else if (initType) {
        if (!declared->assignableFrom(initType)) {
            error(s->init->line, s->init->column, 1,
                  "Cannot assign value of type '" + initType->toString() +
                  "' to variable of type '" + declared->toString() + "'");
        }
    }

    Symbol* sym = makeSymbol(SymbolKind::Variable, s->name, finalType, s->line, s->column);
    if (!currentScope->define(sym)) {
        error(s->line, s->column, static_cast<int>(s->name.size()),
              "Variable '" + asciiOf(s->name) + "' is already defined in this scope");
    }
    s->resolvedSymbol = sym;
}

void Analyzer::analyzeIf(IfStmt* s) {
    Type* condType = analyzeExpr(s->condition.get());
    if (!condType->isError() && !condType->isBool()) {
        error(s->condition->line, s->condition->column, 1,
              "If condition must be 'bool', got '" + condType->toString() + "'");
    }
    analyzeStmt(s->thenBranch.get());
    if (s->elseBranch) analyzeStmt(s->elseBranch.get());
}

void Analyzer::analyzeWhile(WhileStmt* s) {
    Type* condType = analyzeExpr(s->condition.get());
    if (!condType->isError() && !condType->isBool()) {
        error(s->condition->line, s->condition->column, 1,
              "While condition must be 'bool', got '" + condType->toString() + "'");
    }
    analyzeStmt(s->body.get());
}

void Analyzer::analyzeReturn(ReturnStmt* s) {
    if (!currentFunction) {
        error(s->line, s->column, 6, "'return' outside of a function");
        return;
    }
    Type* expected = currentFunction->returnType;
    if (!s->expr) {
        if (expected && !expected->isVoid()) {
            error(s->line, s->column, 6,
                  "Function returns '" + expected->toString() + "', but 'return' has no value");
        }
        return;
    }
    Type* actual = analyzeExpr(s->expr.get());
    if (expected && !expected->isVoid()) {
        if (!expected->assignableFrom(actual)) {
            error(s->expr->line, s->expr->column, 1,
                  "Cannot return value of type '" + actual->toString() +
                  "' from function returning '" + expected->toString() + "'");
        }
    } else if (expected && expected->isVoid()) {
        error(s->expr->line, s->expr->column, 1,
              "Function returns 'void' but 'return' has a value");
    }
}

void Analyzer::analyzeExprStmt(ExprStmt* s) {
    analyzeExpr(s->expr.get());
}

Type* Analyzer::analyzeExpr(Expr* e) {
    Type* t = nullptr;
    if (auto* lit = dynamic_cast<IntLitExpr*>(e))      t = typeCtx.getPrimitive(TypeKind::Int);
    else if (dynamic_cast<DoubleLitExpr*>(e))          t = typeCtx.getPrimitive(TypeKind::Double);
    else if (dynamic_cast<StringLitExpr*>(e))          t = typeCtx.getPrimitive(TypeKind::String);
    else if (dynamic_cast<BoolLitExpr*>(e))            t = typeCtx.getPrimitive(TypeKind::Bool);
    else if (dynamic_cast<NullLitExpr*>(e))            t = typeCtx.getNull();
    else if (auto* id = dynamic_cast<IdentExpr*>(e))   t = analyzeIdent(id);
    else if (auto* th = dynamic_cast<ThisExpr*>(e))    t = analyzeThis(th);
    else if (auto* b = dynamic_cast<BinaryExpr*>(e))   t = analyzeBinary(b);
    else if (auto* u = dynamic_cast<UnaryExpr*>(e))    t = analyzeUnary(u);
    else if (auto* c = dynamic_cast<CallExpr*>(e))     t = analyzeCall(c);
    else if (auto* m = dynamic_cast<MemberExpr*>(e))   t = analyzeMember(m);
    else if (auto* a = dynamic_cast<AssignExpr*>(e))   t = analyzeAssign(a);
    else if (auto* sub = dynamic_cast<SubscriptExpr*>(e)) t = analyzeSubscript(sub);
    else if (auto* tern = dynamic_cast<TernaryExpr*>(e))  t = analyzeTernary(tern);
    else if (auto* nw = dynamic_cast<NewExpr*>(e))        t = analyzeNew(nw);
    else                                                t = typeCtx.getError();
    e->resolvedType = t;
    return t;
}

Type* Analyzer::analyzeIdent(IdentExpr* e) {
    Symbol* sym = currentScope ? currentScope->lookup(e->name) : nullptr;
    if (!sym) {
        error(e->line, e->column, static_cast<int>(e->name.size()),
              "Undefined name '" + asciiOf(e->name) + "'");
        return typeCtx.getError();
    }
    e->resolvedSymbol = sym;
    if (sym->kind == SymbolKind::Function) {
        // Bare function reference — treat as the function itself; type checking handled at call site
        return typeCtx.getError();  // not callable as a value yet
    }
    return sym->type ? sym->type : typeCtx.getError();
}

Type* Analyzer::analyzeThis(ThisExpr* e) {
    if (!currentThis) {
        error(e->line, e->column, 4,
              "'this' is only valid inside a method");
        return typeCtx.getError();
    }
    e->resolvedSymbol = currentThis;
    return currentThis->type ? currentThis->type : typeCtx.getError();
}

Type* Analyzer::analyzeBinary(BinaryExpr* e) {
    Type* l = analyzeExpr(e->left.get());
    Type* r = analyzeExpr(e->right.get());
    if (l->isError() || r->isError()) return typeCtx.getError();

    TokenType op = e->op;
    switch (op) {
        case TokenType::PLUS:
        case TokenType::SUB:
        case TokenType::STAR:
        case TokenType::SLASH:
        case TokenType::PERCENT:
            if (!l->isNumeric() || !r->isNumeric()) {
                error(e->line, e->column, 1,
                      "Operator requires numeric operands, got '" + l->toString() + "' and '" + r->toString() + "'");
                return typeCtx.getError();
            }
            if (!l->equals(r)) {
                error(e->line, e->column, 1,
                      "Operands must be the same type, got '" + l->toString() + "' and '" + r->toString() + "'");
                return typeCtx.getError();
            }
            return l;

        case TokenType::EQ_EQ:
        case TokenType::NOT_EQ:
            if (!l->assignableFrom(r) && !r->assignableFrom(l)) {
                error(e->line, e->column, 1,
                      "Cannot compare '" + l->toString() + "' and '" + r->toString() + "'");
            }
            return typeCtx.getPrimitive(TypeKind::Bool);

        case TokenType::LT:
        case TokenType::GT:
        case TokenType::LT_EQ:
        case TokenType::GT_EQ:
            if (!l->isNumeric() || !r->isNumeric() || !l->equals(r)) {
                error(e->line, e->column, 1,
                      "Comparison requires matching numeric operands, got '" + l->toString() + "' and '" + r->toString() + "'");
            }
            return typeCtx.getPrimitive(TypeKind::Bool);

        case TokenType::AND:
        case TokenType::OR:
            if (!l->isBool() || !r->isBool()) {
                error(e->line, e->column, 1,
                      "Logical operator requires bool operands, got '" + l->toString() + "' and '" + r->toString() + "'");
            }
            return typeCtx.getPrimitive(TypeKind::Bool);

        case TokenType::BIT_AND:
        case TokenType::BIT_OR:
        case TokenType::CARET:
        case TokenType::LT_LT:
        case TokenType::GT_GT:
        case TokenType::GT_GT_GT:
            if (!l->isInteger() || !r->isInteger() || !l->equals(r)) {
                error(e->line, e->column, 1,
                      "Bitwise operator requires matching integer operands, got '" + l->toString() + "' and '" + r->toString() + "'");
            }
            return l;

        default:
            error(e->line, e->column, 1, "Unsupported binary operator");
            return typeCtx.getError();
    }
}

Type* Analyzer::analyzeUnary(UnaryExpr* e) {
    Type* t = analyzeExpr(e->operand.get());
    if (t->isError()) return typeCtx.getError();
    switch (e->op) {
        case TokenType::SUB:
            if (!t->isNumeric()) {
                error(e->line, e->column, 1,
                      "Unary '-' requires numeric, got '" + t->toString() + "'");
                return typeCtx.getError();
            }
            return t;
        case TokenType::NOT:
            if (!t->isBool()) {
                error(e->line, e->column, 1,
                      "Unary '!' requires bool, got '" + t->toString() + "'");
                return typeCtx.getError();
            }
            return t;
        case TokenType::PLUS_PLUS:
        case TokenType::SUB_SUB:
            if (!t->isNumeric()) {
                error(e->line, e->column, 1,
                      "Increment/decrement requires numeric, got '" + t->toString() + "'");
                return typeCtx.getError();
            }
            if (!isLValue(e->operand.get())) {
                error(e->line, e->column, 1, "Cannot increment/decrement a non-assignable expression");
            }
            return t;
        default:
            error(e->line, e->column, 1, "Unsupported unary operator");
            return typeCtx.getError();
    }
}

Type* Analyzer::analyzeCall(CallExpr* e) {
    // `obj.method(args)` — method call on a struct
    if (auto* member = dynamic_cast<MemberExpr*>(e->callee.get())) {
        analyzeExpr(e->callee.get());  // resolves field-or-method on member
        if (member->resolvedMethodSymbol) {
            Symbol* sym = member->resolvedMethodSymbol;
            size_t requiredCount = sym->paramTypes.size();
            if (sym->funcDecl) {
                requiredCount = 0;
                for (auto& p : sym->funcDecl->parameters) {
                    if (p.defaultValue) break;
                    requiredCount++;
                }
            }
            if (e->args.size() < requiredCount || e->args.size() > sym->paramTypes.size()) {
                error(e->line, e->column, 1,
                      "Method '" + asciiOf(member->member) + "' expects " +
                      std::to_string(requiredCount) + (requiredCount == sym->paramTypes.size() ? "" : "-" + std::to_string(sym->paramTypes.size())) +
                      " argument(s), got " + std::to_string(e->args.size()));
            }
            size_t n = std::min(e->args.size(), sym->paramTypes.size());
            for (size_t i = 0; i < n; ++i) {
                Type* argT = analyzeExpr(e->args[i].get());
                Type* paramT = sym->paramTypes[i];
                if (!paramT->assignableFrom(argT)) {
                    error(e->args[i]->line, e->args[i]->column, 1,
                          "Argument " + std::to_string(i + 1) + ": expected '" +
                          paramT->toString() + "', got '" + argT->toString() + "'");
                }
            }
            for (size_t i = n; i < e->args.size(); ++i) analyzeExpr(e->args[i].get());
            return sym->returnType ? sym->returnType : typeCtx.getError();
        }
        // analyzeMember already emitted an error if the name didn't resolve.
        for (auto& a : e->args) analyzeExpr(a.get());
        return typeCtx.getError();
    }

    auto* idCallee = dynamic_cast<IdentExpr*>(e->callee.get());
    if (!idCallee) {
        error(e->line, e->column, 1, "Only direct function calls are supported");
        for (auto& a : e->args) analyzeExpr(a.get());
        return typeCtx.getError();
    }

    Symbol* sym = currentScope ? currentScope->lookup(idCallee->name) : nullptr;
    if (!sym) {
        error(idCallee->line, idCallee->column, static_cast<int>(idCallee->name.size()),
              "Undefined function '" + asciiOf(idCallee->name) + "'");
        for (auto& a : e->args) analyzeExpr(a.get());
        return typeCtx.getError();
    }
    if (sym->kind != SymbolKind::Function) {
        error(idCallee->line, idCallee->column, static_cast<int>(idCallee->name.size()),
              "'" + asciiOf(idCallee->name) + "' is not a function");
        for (auto& a : e->args) analyzeExpr(a.get());
        return typeCtx.getError();
    }
    idCallee->resolvedSymbol = sym;

    size_t requiredCount = sym->paramTypes.size();
    if (sym->funcDecl) {
        requiredCount = 0;
        for (auto& p : sym->funcDecl->parameters) {
            if (p.defaultValue) break;
            requiredCount++;
        }
    }
    if (e->args.size() < requiredCount || e->args.size() > sym->paramTypes.size()) {
        error(e->line, e->column, 1,
              "Function '" + asciiOf(idCallee->name) + "' expects " +
              std::to_string(requiredCount) + (requiredCount == sym->paramTypes.size() ? "" : "-" + std::to_string(sym->paramTypes.size())) +
              " argument(s), got " + std::to_string(e->args.size()));
    }

    size_t n = std::min(e->args.size(), sym->paramTypes.size());
    for (size_t i = 0; i < n; ++i) {
        Type* argT = analyzeExpr(e->args[i].get());
        Type* paramT = sym->paramTypes[i];
        if (!paramT->assignableFrom(argT)) {
            error(e->args[i]->line, e->args[i]->column, 1,
                  "Argument " + std::to_string(i + 1) + ": expected '" +
                  paramT->toString() + "', got '" + argT->toString() + "'");
        }
    }
    for (size_t i = n; i < e->args.size(); ++i) analyzeExpr(e->args[i].get());

    return sym->returnType ? sym->returnType : typeCtx.getError();
}

Type* Analyzer::analyzeMember(MemberExpr* e) {
    Type* objT = analyzeExpr(e->object.get());
    if (objT->isError()) return typeCtx.getError();
    if (!objT->hasRecordLayout() || !objT->structInfo) {
        error(e->line, e->column, 1,
              "Member access on non-record type '" + objT->toString() + "'");
        return typeCtx.getError();
    }
    int idx = objT->structInfo->findFieldIndex(e->member);
    if (idx >= 0) {
        return objT->structInfo->fields[idx].type;
    }
    int midx = objT->structInfo->findMethodIndex(e->member);
    if (midx >= 0) {
        // Method reference: only valid as the callee of a CallExpr. The caller
        // (analyzeCall) checks resolvedMethodSymbol and handles the call.
        e->resolvedMethodSymbol = objT->structInfo->methods[midx].symbol;
        return typeCtx.getError();  // not a value type
    }
    error(e->line, e->column, static_cast<int>(e->member.size()),
          "No field or method '" + asciiOf(e->member) + "' on type '" + objT->toString() + "'");
    return typeCtx.getError();
}

Type* Analyzer::analyzeNew(NewExpr* e) {
    Type* t = typeCtx.lookupClass(e->typeName);
    if (!t) {
        if (typeCtx.lookupStruct(e->typeName)) {
            error(e->line, e->column, static_cast<int>(e->typeName.size()),
                  "'new' is only valid for classes; '" + asciiOf(e->typeName) + "' is a struct");
        } else {
            error(e->line, e->column, static_cast<int>(e->typeName.size()),
                  "Unknown class '" + asciiOf(e->typeName) + "'");
        }
        for (auto& a : e->args) analyzeExpr(a.get());
        return typeCtx.getError();
    }
    e->resolvedClassType = t;

    // A constructor is a method whose name matches the class name. Look it up
    // so we can validate the argument list at the `new` site.
    Symbol* ctor = nullptr;
    int ctorIdx = t->structInfo->findMethodIndex(t->structInfo->name);
    if (ctorIdx >= 0) ctor = t->structInfo->methods[ctorIdx].symbol;

    if (ctor) {
        size_t requiredCount = ctor->paramTypes.size();
        if (ctor->funcDecl) {
            requiredCount = 0;
            for (auto& p : ctor->funcDecl->parameters) {
                if (p.defaultValue) break;
                requiredCount++;
            }
        }
        if (e->args.size() < requiredCount || e->args.size() > ctor->paramTypes.size()) {
            error(e->line, e->column, 1,
                  "Constructor '" + asciiOf(e->typeName) + "' expects " +
                  std::to_string(requiredCount) + (requiredCount == ctor->paramTypes.size() ? "" : "-" + std::to_string(ctor->paramTypes.size())) +
                  " argument(s), got " + std::to_string(e->args.size()));
        }
        size_t n = std::min(e->args.size(), ctor->paramTypes.size());
        for (size_t i = 0; i < n; ++i) {
            Type* argT = analyzeExpr(e->args[i].get());
            Type* paramT = ctor->paramTypes[i];
            if (!paramT->assignableFrom(argT)) {
                error(e->args[i]->line, e->args[i]->column, 1,
                      "Argument " + std::to_string(i + 1) + ": expected '" +
                      paramT->toString() + "', got '" + argT->toString() + "'");
            }
        }
        for (size_t i = n; i < e->args.size(); ++i) analyzeExpr(e->args[i].get());
    } else {
        // No constructor declared. `new ClassName()` is fine; passing args is not.
        if (!e->args.empty()) {
            error(e->line, e->column, 1,
                  "Class '" + asciiOf(e->typeName) + "' has no constructor; use 'new " +
                  asciiOf(e->typeName) + "()'");
            for (auto& a : e->args) analyzeExpr(a.get());
        }
    }
    return t;
}

Type* Analyzer::analyzeAssign(AssignExpr* e) {
    if (!isLValue(e->target.get())) {
        error(e->line, e->column, 1, "Left side of assignment must be an assignable expression");
    }
    Type* targetT = analyzeExpr(e->target.get());
    Type* valueT = analyzeExpr(e->value.get());
    if (!targetT->isError() && !valueT->isError()) {
        if (!targetT->assignableFrom(valueT)) {
            error(e->line, e->column, 1,
                  "Cannot assign '" + valueT->toString() + "' to '" + targetT->toString() + "'");
        }
    }
    return targetT;
}

Type* Analyzer::analyzeSubscript(SubscriptExpr* e) {
    analyzeExpr(e->object.get());
    analyzeExpr(e->index.get());
    error(e->line, e->column, 1, "Subscript access is not yet supported");
    return typeCtx.getError();
}

Type* Analyzer::analyzeTernary(TernaryExpr* e) {
    Type* condT = analyzeExpr(e->cond.get());
    Type* thenT = analyzeExpr(e->thenExpr.get());
    Type* elseT = analyzeExpr(e->elseExpr.get());

    if (!condT->isError() && !condT->isBool()) {
        error(e->cond->line, e->cond->column, 1,
              "Ternary condition must be 'bool', got '" + condT->toString() + "'");
    }
    if (thenT->isError() || elseT->isError()) return typeCtx.getError();
    if (thenT->equals(elseT)) return thenT;
    if (thenT->assignableFrom(elseT)) return thenT;
    if (elseT->assignableFrom(thenT)) return elseT;
    error(e->line, e->column, 1,
          "Ternary branches have incompatible types '" + thenT->toString() +
          "' and '" + elseT->toString() + "'");
    return typeCtx.getError();
}

bool Analyzer::isLValue(Expr* e) {
    if (dynamic_cast<IdentExpr*>(e)) return true;
    if (dynamic_cast<MemberExpr*>(e)) return true;
    if (dynamic_cast<SubscriptExpr*>(e)) return true;
    return false;
}
