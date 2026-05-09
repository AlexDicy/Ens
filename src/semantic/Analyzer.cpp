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
    collectFunctions(program);
    for (const auto& s : program) {
        if (auto* fn = dynamic_cast<FuncDecl*>(s.get())) {
            analyzeFunctionBody(fn);
        } else if (dynamic_cast<StructDecl*>(s.get())) {
            // Already collected; struct bodies are pure declarations
        } else {
            // Top-level statement that isn't a function — analyze it directly
            analyzeStmt(s.get());
        }
    }
}

void Analyzer::collectStructs(const std::vector<StmtPtr>& program) {
    // First pass: register the names so structs can reference each other.
    for (const auto& s : program) {
        auto* sd = dynamic_cast<StructDecl*>(s.get());
        if (!sd) continue;
        if (typeCtx.lookupStruct(sd->name)) {
            error(sd->line, sd->column, static_cast<int>(sd->name.size()),
                  "Duplicate struct '" + asciiOf(sd->name) + "'");
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
}

void Analyzer::collectFunctions(const std::vector<StmtPtr>& program) {
    for (const auto& s : program) {
        auto* fn = dynamic_cast<FuncDecl*>(s.get());
        if (!fn) continue;

        Type* retType = fn->returnType ? resolveTypeNode(fn->returnType.get())
                                       : typeCtx.getPrimitive(TypeKind::Void);

        Symbol* sym = makeSymbol(SymbolKind::Function, fn->name, nullptr, fn->line, fn->column);
        sym->returnType = retType;
        for (auto& p : fn->parameters) {
            Type* pt = resolveTypeNode(p.type.get());
            sym->paramTypes.push_back(pt);
        }
        if (!globalScope->define(sym)) {
            error(fn->line, fn->column, static_cast<int>(fn->name.size()),
                  "Duplicate function name '" + asciiOf(fn->name) + "'");
        }
        fn->resolvedSymbol = sym;
    }
}

void Analyzer::analyzeFunctionBody(FuncDecl* fn) {
    Symbol* prevFunction = currentFunction;
    currentFunction = fn->resolvedSymbol;

    pushScope();
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
    else if (auto* b = dynamic_cast<BinaryExpr*>(e))   t = analyzeBinary(b);
    else if (auto* u = dynamic_cast<UnaryExpr*>(e))    t = analyzeUnary(u);
    else if (auto* c = dynamic_cast<CallExpr*>(e))     t = analyzeCall(c);
    else if (auto* m = dynamic_cast<MemberExpr*>(e))   t = analyzeMember(m);
    else if (auto* a = dynamic_cast<AssignExpr*>(e))   t = analyzeAssign(a);
    else if (auto* sub = dynamic_cast<SubscriptExpr*>(e)) t = analyzeSubscript(sub);
    else if (auto* tern = dynamic_cast<TernaryExpr*>(e))  t = analyzeTernary(tern);
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

    if (e->args.size() != sym->paramTypes.size()) {
        error(e->line, e->column, 1,
              "Function '" + asciiOf(idCallee->name) + "' expects " +
              std::to_string(sym->paramTypes.size()) + " argument(s), got " +
              std::to_string(e->args.size()));
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
    if (!objT->isStruct() || !objT->structInfo) {
        error(e->line, e->column, 1,
              "Member access on non-struct type '" + objT->toString() + "'");
        return typeCtx.getError();
    }
    int idx = objT->structInfo->findFieldIndex(e->member);
    if (idx < 0) {
        error(e->line, e->column, static_cast<int>(e->member.size()),
              "No field '" + asciiOf(e->member) + "' on type '" + objT->toString() + "'");
        return typeCtx.getError();
    }
    return objT->structInfo->fields[idx].type;
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
