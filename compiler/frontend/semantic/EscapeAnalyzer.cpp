#include "EscapeAnalyzer.h"

#include "Type.h"

EscapeAnalyzer::EscapeAnalyzer(const ast::SourceFile& sourceFile, const AnalysisResult& a)
    : sf(sourceFile), analysis(a) {}

bool EscapeAnalyzer::runOnce() {
    changedThisIteration = false;
    for (auto& fn : sf.functions()) {
        auto* info = analysis.find(fn.node.greenNode());
        if (info && info->resolvedSymbol) analyzeFunction(info->resolvedSymbol, fn);
    }
    for (auto& sd : sf.structs()) {
        for (auto& m : sd.methods()) {
            auto* info = analysis.find(m.node.greenNode());
            if (info && info->resolvedSymbol) analyzeFunction(info->resolvedSymbol, m);
        }
    }
    for (auto& cd : sf.classes()) {
        for (auto& m : cd.methods()) {
            auto* info = analysis.find(m.node.greenNode());
            if (info && info->resolvedSymbol) analyzeFunction(info->resolvedSymbol, m);
        }
    }
    return changedThisIteration;
}

void EscapeAnalyzer::analyzeFunction(Symbol* fnSym, const ast::FuncDecl& fn) {
    currentFn = fnSym;

    currentParams.clear();
    auto params = fn.parameters();
    for (auto& p : params) {
        auto* info = analysis.find(p.node.greenNode());
        currentParams.push_back(info ? info->resolvedSymbol : nullptr);
    }

    auto& ei = fnSym->escapeInfo;
    if (ei.params.size() != currentParams.size()) {
        ei.params.assign(currentParams.size(), EscapeKind::NoEscape);
        ei.paramMutated.assign(currentParams.size(), false);
    }

    if (auto body = fn.body()) {
        scanBlock(*body);
    }
    for (auto& cc : fn.catchClauses()) {
        if (auto cb = cc.body()) scanBlock(*cb);
    }

    ei.analyzed = true;
    currentFn = nullptr;
    currentParams.clear();
}

void EscapeAnalyzer::scanStatement(const ast::Statement& s) {
    if (auto b = s.asBlock()) { scanBlock(*b); return; }
    if (auto l = s.asLet()) { scanLetStmt(*l); return; }
    if (auto v = s.asTypedVarDecl()) { scanTypedVarDecl(*v); return; }
    if (auto i = s.asIf()) { scanIf(*i); return; }
    if (auto w = s.asWhile()) { scanWhile(*w); return; }
    if (auto f = s.asFor()) { scanFor(*f); return; }
    if (auto fe = s.asForEach()) { scanForEach(*fe); return; }
    if (s.asBreak() || s.asContinue()) return;
    if (auto r = s.asReturn()) { scanReturn(*r); return; }
    if (auto e = s.asExpressionStmt()) { scanExprStmt(*e); return; }
    if (auto th = s.asThrow()) {
        if (auto v = th->value()) { markEscapeIfRef(*v); scanExpression(*v); }
        return;
    }
    if (s.asRethrow()) return;
}

void EscapeAnalyzer::scanBlock(const ast::Block& b) {
    for (auto& child : b.statements()) {
        scanStatement(child);
    }
}

void EscapeAnalyzer::scanLetStmt(const ast::LetStatement& s) {
    Symbol* letSym = nullptr;
    if (auto* info = analysis.find(s.node.greenNode())) letSym = info->resolvedSymbol;

    if (auto init = s.initializer()) {
        if (letSym) {
            if (auto id = init->asIdent()) {
                auto* iinfo = analysis.find(id->node.greenNode());
                Symbol* src = iinfo ? iinfo->resolvedSymbol : nullptr;
                if (src) letSym->aliasOf = src;
            }
            updateBorrowMode(letSym, *init);
        }
        scanExpression(*init);
    }
}

void EscapeAnalyzer::scanTypedVarDecl(const ast::TypedVarDeclStatement& s) {
    Symbol* letSym = nullptr;
    if (auto* info = analysis.find(s.node.greenNode())) letSym = info->resolvedSymbol;

    if (auto init = s.initializer()) {
        if (letSym) {
            if (auto id = init->asIdent()) {
                auto* iinfo = analysis.find(id->node.greenNode());
                Symbol* src = iinfo ? iinfo->resolvedSymbol : nullptr;
                if (src) letSym->aliasOf = src;
            }
            updateBorrowMode(letSym, *init);
        }
        scanExpression(*init);
    } else if (letSym) {
        // Initialized to default (zero / null). Not a parameter borrow source.
        if (letSym->allAssignsFromParam) {
            letSym->allAssignsFromParam = false;
            changedThisIteration = true;
        }
    }
}

void EscapeAnalyzer::scanIf(const ast::IfStatement& s) {
    if (auto c = s.condition()) scanExpression(*c);
    if (auto then = s.thenBlock()) scanBlock(*then);
    if (auto ec = s.elseClause()) {
        if (auto innerIf = ec->ifStatement()) scanIf(*innerIf);
        else if (auto bb = ec->block()) scanBlock(*bb);
    }
}

void EscapeAnalyzer::scanWhile(const ast::WhileStatement& s) {
    if (auto c = s.condition()) scanExpression(*c);
    if (auto body = s.body()) scanBlock(*body);
}

void EscapeAnalyzer::scanFor(const ast::ForStatement& s) {
    if (auto init = s.init()) scanStatement(*init);
    if (auto c = s.condition()) scanExpression(*c);
    if (auto u = s.update()) scanExpression(*u);
    if (auto body = s.body()) scanBlock(*body);
}

void EscapeAnalyzer::scanForEach(const ast::ForEachStatement& s) {
    if (auto it = s.iterable()) scanExpression(*it);
    if (auto body = s.body()) scanBlock(*body);
}

void EscapeAnalyzer::scanReturn(const ast::ReturnStatement& s) {
    if (auto v = s.value()) {
        markEscapeIfRef(*v);
        scanExpression(*v);
    }
}

void EscapeAnalyzer::scanExprStmt(const ast::ExpressionStatement& s) {
    if (auto e = s.expression()) scanExpression(*e);
}

void EscapeAnalyzer::scanExpression(const ast::Expression& e) {
    if (auto a = e.asAssign()) { scanAssign(*a); return; }
    if (auto c = e.asCall()) { scanCall(*c); return; }
    if (auto m = e.asMember()) { scanMember(*m); return; }
    if (auto sm = e.asSafeMember()) {
        if (auto obj = sm->object()) scanExpression(*obj);
        return;
    }
    if (auto su = e.asSubscript()) { scanSubscript(*su); return; }
    if (auto ss = e.asSafeSubscript()) {
        if (auto obj = ss->object()) scanExpression(*obj);
        if (auto idx = ss->index()) scanExpression(*idx);
        return;
    }
    if (auto c = e.asCast()) {
        if (auto src = c->source()) scanExpression(*src);
        return;
    }
    if (e.asOutArgument()) {
        // External calls don't participate in ARC-aware escape analysis; the
        // referenced local is treated as reassigned by the analyzer itself.
        return;
    }
    if (auto bn = e.asBinary()) { scanBinary(*bn); return; }
    if (auto t = e.asTernary()) { scanTernary(*t); return; }
    if (auto nc = e.asNullCoalesce()) {
        if (auto l = nc->left()) scanExpression(*l);
        if (auto r = nc->right()) scanExpression(*r);
        return;
    }
    if (auto p = e.asParen()) { scanParen(*p); return; }
    if (auto n = e.asNew()) { scanNew(*n); return; }
    if (auto al = e.asArrayLiteral()) { scanArrayLiteral(*al); return; }
    if (auto tr = e.asTry()) { if (auto op = tr->operand()) scanExpression(*op); return; }
    if (auto id = e.asIdent()) { scanIdent(*id); return; }
}

void EscapeAnalyzer::scanIdent(const ast::IdentExpression&) {}

void EscapeAnalyzer::scanAssign(const ast::AssignExpression& e) {
    auto target = e.target();
    auto value = e.value();
    if (!target || !value) return;

    if (auto m = target->asMember()) {
        // Walk to root of member chain.
        std::optional<ast::Expression> cursor = m->object();
        while (cursor) {
            if (auto innerMem = cursor->asMember()) {
                cursor = innerMem->object();
            } else {
                break;
            }
        }
        if (cursor) {
            if (auto rootId = cursor->asIdent()) {
                auto* info = analysis.find(rootId->node.greenNode());
                Symbol* s = info ? info->resolvedSymbol : nullptr;
                Symbol* root = aliasRoot(s);
                int idx = paramIndexOfSymbol(root);
                if (idx >= 0) markParamMutated(idx);
                // Mark every Variable in the alias chain as structFieldsMutated so
                // struct-borrow elision is disabled for any let-var that aliases a
                // mutated source. Without this, `let y = c; y.b = new Box()` keeps
                // y as a no-retain borrow of c, releasing c.b through y's mutation
                // and leaving a dangling pointer for c's eventual cleanup.
                for (Symbol* cur = s; cur && cur->kind == SymbolKind::Variable; cur = cur->aliasOf) {
                    if (!cur->structFieldsMutated) {
                        cur->structFieldsMutated = true;
                        changedThisIteration = true;
                    }
                }
            }
        }
        markEscapeIfRef(*value);
    } else if (auto id = target->asIdent()) {
        auto* info = analysis.find(id->node.greenNode());
        Symbol* targetSym = info ? info->resolvedSymbol : nullptr;
        if (targetSym) {
            markSymbolReassigned(targetSym);
            // Reassignment invalidates any previous alias relationship.
            if (targetSym->aliasOf) targetSym->aliasOf = nullptr;
            updateBorrowMode(targetSym, *value);
        }
        if (!isBorrowModeSymbol(targetSym)) {
            markEscapeIfRef(*value);
        }
    } else if (target->asSubscript()) {
        markEscapeIfRef(*value);
    }

    scanExpression(*target);
    scanExpression(*value);
}

void EscapeAnalyzer::scanSubscript(const ast::SubscriptExpression& e) {
    if (auto obj = e.object()) scanExpression(*obj);
    if (auto idx = e.index()) scanExpression(*idx);
}

void EscapeAnalyzer::scanCall(const ast::CallExpression& e) {
    auto callee = e.callee();
    Symbol* calleeSym = nullptr;
    if (callee) {
        if (auto member = callee->asMember()) {
            auto* info = analysis.find(member->node.greenNode());
            // Methods use resolvedMethodSymbol; namespace-qualified free-function calls
            // (ns.func) store the function in resolvedSymbol.
            calleeSym = info ? (info->resolvedMethodSymbol ? info->resolvedMethodSymbol
                                                           : info->resolvedSymbol)
                             : nullptr;
            if (auto obj = member->object()) scanExpression(*obj);
        } else if (auto id = callee->asIdent()) {
            auto* info = analysis.find(id->node.greenNode());
            calleeSym = info ? info->resolvedSymbol : nullptr;
        }
    }

    auto args = e.arguments();
    for (size_t i = 0; i < args.size(); ++i) {
        if (auto id = args[i].asIdent()) {
            auto* info = analysis.find(id->node.greenNode());
            Symbol* argSym = info ? info->resolvedSymbol : nullptr;
            bool calleeEscapesParam = true;
            if (calleeSym && i < calleeSym->escapeInfo.params.size()) {
                calleeEscapesParam = (calleeSym->escapeInfo.params[i] == EscapeKind::Escape);
            }
            if (argSym && calleeEscapesParam) {
                markSymbolEscape(argSym);
            }
        } else {
            scanExpression(args[i]);
        }
    }
}

void EscapeAnalyzer::scanMember(const ast::MemberExpression& e) {
    if (auto obj = e.object()) scanExpression(*obj);
}

void EscapeAnalyzer::scanBinary(const ast::BinaryExpression& e) {
    if (auto l = e.left()) scanExpression(*l);
    if (auto r = e.right()) scanExpression(*r);
}

void EscapeAnalyzer::scanTernary(const ast::TernaryExpression& e) {
    if (auto c = e.condition()) scanExpression(*c);
    if (auto t = e.thenBranch()) scanExpression(*t);
    if (auto el = e.elseBranch()) scanExpression(*el);
}

void EscapeAnalyzer::scanParen(const ast::ParenExpression& e) {
    if (auto inner = e.inner()) scanExpression(*inner);
}

void EscapeAnalyzer::scanNew(const ast::NewExpression& e) {
    for (auto& arg : e.arguments()) {
        scanExpression(arg);
    }
}

void EscapeAnalyzer::scanArrayLiteral(const ast::ArrayLiteralExpression& e) {
    for (auto& el : e.elements()) {
        markEscapeIfRef(el);
        scanExpression(el);
    }
}

void EscapeAnalyzer::markEscapeIfRef(const ast::Expression& e) {
    if (auto id = e.asIdent()) {
        auto* info = analysis.find(id->node.greenNode());
        Symbol* s = info ? info->resolvedSymbol : nullptr;
        if (s) markSymbolEscape(s);
        return;
    }
    if (auto p = e.asParen()) {
        if (auto inner = p->inner()) markEscapeIfRef(*inner);
        return;
    }
    if (auto t = e.asTernary()) {
        if (auto th = t->thenBranch()) markEscapeIfRef(*th);
        if (auto el = t->elseBranch()) markEscapeIfRef(*el);
        return;
    }
}

int EscapeAnalyzer::paramIndexOfSymbol(Symbol* sym) const {
    if (!sym) return -1;
    for (size_t i = 0; i < currentParams.size(); ++i) {
        if (currentParams[i] == sym) return static_cast<int>(i);
    }
    return -1;
}

Symbol* EscapeAnalyzer::aliasRoot(Symbol* sym) const {
    while (sym && sym->aliasOf) sym = sym->aliasOf;
    return sym;
}

void EscapeAnalyzer::markSymbolEscape(Symbol* sym) {
    // Propagate Escape through the alias chain.
    Symbol* cur = sym;
    while (cur) {
        if (cur->localEscape != EscapeKind::Escape) {
            cur->localEscape = EscapeKind::Escape;
            changedThisIteration = true;
            // Mirror to function-level info if this is a parameter of currentFn.
            int idx = paramIndexOfSymbol(cur);
            if (idx >= 0 && currentFn) {
                auto& ei = currentFn->escapeInfo;
                if (idx < static_cast<int>(ei.params.size()) && ei.params[idx] != EscapeKind::Escape) {
                    ei.params[idx] = EscapeKind::Escape;
                }
            }
        } else {
            break;
        }
        cur = cur->aliasOf;
    }
}

void EscapeAnalyzer::markSymbolReassigned(Symbol* sym) {
    if (!sym) return;
    if (!sym->reassigned) {
        sym->reassigned = true;
        changedThisIteration = true;
    }
}

bool EscapeAnalyzer::isParameterBorrowSource(const ast::Expression& e) const {
    if (auto id = e.asIdent()) {
        auto* info = analysis.find(id->node.greenNode());
        Symbol* s = info ? info->resolvedSymbol : nullptr;
        if (!s) return false;
        if (s->kind != SymbolKind::Parameter) return false;
        if (!s->type || s->type->kind != TypeKind::Class) {
            // Allow Optional<Class> params too.
            if (!s->type || s->type->kind != TypeKind::Optional ||
                !s->type->inner || !s->type->inner->isClass()) {
                return false;
            }
        }
        if (s->reassigned) return false;
        return true;
    }
    if (e.asThis()) return true;
    if (auto p = e.asParen()) {
        if (auto inner = p->inner()) return isParameterBorrowSource(*inner);
    }
    return false;
}

void EscapeAnalyzer::updateBorrowMode(Symbol* target, const ast::Expression& rhs) {
    if (!target) return;
    if (!target->allAssignsFromParam) return;  // already false; nothing to track
    if (!isParameterBorrowSource(rhs)) {
        target->allAssignsFromParam = false;
        changedThisIteration = true;
    }
}

bool EscapeAnalyzer::isBorrowModeSymbol(Symbol* sym) const {
    if (!sym || sym->kind != SymbolKind::Variable) return false;
    if (!sym->type) return false;
    bool isClass = (sym->type->kind == TypeKind::Class) ||
        (sym->type->kind == TypeKind::Optional && sym->type->inner && sym->type->inner->isClass());
    if (!isClass) return false;
    if (sym->localEscape != EscapeKind::NoEscape) return false;
    if (!sym->allAssignsFromParam) return false;
    return true;
}

void EscapeAnalyzer::finalize() {
    auto handle = [&](const ast::FuncDecl& fn) {
        loopDepth = 0;
        walkBodyForLastUses(fn);
    };
    for (auto& fn : sf.functions()) handle(fn);
    for (auto& sd : sf.structs()) for (auto& m : sd.methods()) handle(m);
    for (auto& cd : sf.classes()) for (auto& m : cd.methods()) handle(m);
}

void EscapeAnalyzer::walkBodyForLastUses(const ast::FuncDecl& fn) {
    if (auto body = fn.body()) {
        for (auto& s : body->statements()) walkStmtForLastUses(s);
    }
    for (auto& cc : fn.catchClauses()) {
        if (auto cb = cc.body()) {
            for (auto& s : cb->statements()) walkStmtForLastUses(s);
        }
    }
}

void EscapeAnalyzer::walkStmtForLastUses(const ast::Statement& s) {
    if (auto b = s.asBlock()) {
        for (auto& child : b->statements()) walkStmtForLastUses(child);
        return;
    }
    if (auto l = s.asLet()) {
        if (auto init = l->initializer()) walkExprForLastUses(*init);
        return;
    }
    if (auto v = s.asTypedVarDecl()) {
        if (auto init = v->initializer()) walkExprForLastUses(*init);
        return;
    }
    if (auto i = s.asIf()) {
        if (auto c = i->condition()) walkExprForLastUses(*c);
        if (auto t = i->thenBlock()) {
            for (auto& child : t->statements()) walkStmtForLastUses(child);
        }
        if (auto ec = i->elseClause()) {
            if (auto inner = ec->ifStatement()) {
                // Treat as a statement: wrap in scan
                ast::Statement asStmt{inner->node};
                walkStmtForLastUses(asStmt);
            } else if (auto bb = ec->block()) {
                for (auto& child : bb->statements()) walkStmtForLastUses(child);
            }
        }
        return;
    }
    if (auto w = s.asWhile()) {
        if (auto c = w->condition()) walkExprForLastUses(*c);
        loopDepth++;
        if (auto body = w->body()) {
            for (auto& child : body->statements()) walkStmtForLastUses(child);
        }
        loopDepth--;
        return;
    }
    if (auto f = s.asFor()) {
        if (auto init = f->init()) walkStmtForLastUses(*init);
        if (auto c = f->condition()) walkExprForLastUses(*c);
        loopDepth++;
        if (auto u = f->update()) walkExprForLastUses(*u);
        if (auto body = f->body()) {
            for (auto& child : body->statements()) walkStmtForLastUses(child);
        }
        loopDepth--;
        return;
    }
    if (auto fe = s.asForEach()) {
        if (auto it = fe->iterable()) walkExprForLastUses(*it);
        loopDepth++;
        if (auto body = fe->body()) {
            for (auto& child : body->statements()) walkStmtForLastUses(child);
        }
        loopDepth--;
        return;
    }
    if (s.asBreak() || s.asContinue()) return;
    if (auto r = s.asReturn()) {
        if (auto v = r->value()) walkExprForLastUses(*v);
        return;
    }
    if (auto e = s.asExpressionStmt()) {
        if (auto exp = e->expression()) walkExprForLastUses(*exp);
        return;
    }
    if (auto th = s.asThrow()) {
        if (auto v = th->value()) walkExprForLastUses(*v);
        return;
    }
}

void EscapeAnalyzer::walkExprForLastUses(const ast::Expression& e) {
    if (auto a = e.asAssign()) {
        if (auto target = a->target()) {
            if (auto m = target->asMember()) {
                if (auto obj = m->object()) walkExprForLastUses(*obj);
            }
        }
        if (auto value = a->value()) walkExprForLastUses(*value);
        return;
    }
    if (auto c = e.asCall()) {
        if (auto callee = c->callee()) {
            if (auto m = callee->asMember()) {
                if (auto obj = m->object()) walkExprForLastUses(*obj);
            } else if (auto sm = callee->asSafeMember()) {
                if (auto obj = sm->object()) walkExprForLastUses(*obj);
            }
        }
        for (auto& arg : c->arguments()) walkExprForLastUses(arg);
        return;
    }
    if (auto m = e.asMember()) {
        if (auto obj = m->object()) walkExprForLastUses(*obj);
        return;
    }
    if (auto sm = e.asSafeMember()) {
        if (auto obj = sm->object()) walkExprForLastUses(*obj);
        return;
    }
    if (auto su = e.asSubscript()) {
        if (auto obj = su->object()) walkExprForLastUses(*obj);
        if (auto idx = su->index()) walkExprForLastUses(*idx);
        return;
    }
    if (auto ss = e.asSafeSubscript()) {
        if (auto obj = ss->object()) walkExprForLastUses(*obj);
        if (auto idx = ss->index()) walkExprForLastUses(*idx);
        return;
    }
    if (auto c = e.asCast()) {
        if (auto src = c->source()) walkExprForLastUses(*src);
        return;
    }
    if (auto bn = e.asBinary()) {
        if (auto l = bn->left()) walkExprForLastUses(*l);
        if (auto r = bn->right()) walkExprForLastUses(*r);
        return;
    }
    if (auto t = e.asTernary()) {
        if (auto cond = t->condition()) walkExprForLastUses(*cond);
        if (auto th = t->thenBranch()) walkExprForLastUses(*th);
        if (auto el = t->elseBranch()) walkExprForLastUses(*el);
        return;
    }
    if (auto nc = e.asNullCoalesce()) {
        if (auto l = nc->left()) walkExprForLastUses(*l);
        if (auto r = nc->right()) walkExprForLastUses(*r);
        return;
    }
    if (auto p = e.asParen()) {
        if (auto inner = p->inner()) walkExprForLastUses(*inner);
        return;
    }
    if (auto n = e.asNew()) {
        for (auto& arg : n->arguments()) walkExprForLastUses(arg);
        return;
    }
    if (auto al = e.asArrayLiteral()) {
        for (auto& el : al->elements()) walkExprForLastUses(el);
        return;
    }
    if (auto tr = e.asTry()) {
        if (auto op = tr->operand()) walkExprForLastUses(*op);
        return;
    }
    if (auto id = e.asIdent()) {
        recordRead(*id);
        return;
    }
}

void EscapeAnalyzer::recordRead(const ast::IdentExpression& id) {
    auto* info = analysis.find(id.node.greenNode());
    Symbol* s = info ? info->resolvedSymbol : nullptr;
    if (!s) return;
    if (s->kind != SymbolKind::Variable) return;
    if (!s->type) return;
    bool isClassish = (s->type->kind == TypeKind::Class) ||
        (s->type->kind == TypeKind::Optional && s->type->inner && s->type->inner->isClass());
    if (!isClassish) return;
    s->lastUseRef = id.node.greenNode();
    s->lastUseInLoop = (loopDepth > 0);
}

void EscapeAnalyzer::markParamMutated(int paramIdx) {
    if (!currentFn || paramIdx < 0) return;
    auto& ei = currentFn->escapeInfo;
    if (paramIdx >= static_cast<int>(ei.paramMutated.size())) return;
    if (!ei.paramMutated[paramIdx]) {
        ei.paramMutated[paramIdx] = true;
        changedThisIteration = true;
    }
}

void EscapeAnalyzer::collectFunctionsOnce() {}

void EscapeAnalyzer::decideStackPromotions() {
    auto handle = [&](const ast::FuncDecl& fn) {
        walkBodyForPromotion(fn);
    };
    for (auto& fn : sf.functions()) handle(fn);
    for (auto& sd : sf.structs()) for (auto& m : sd.methods()) handle(m);
    for (auto& cd : sf.classes()) for (auto& m : cd.methods()) handle(m);
}

void EscapeAnalyzer::walkBodyForPromotion(const ast::FuncDecl& fn) {
    if (auto body = fn.body()) {
        for (auto& s : body->statements()) walkStmtForPromotion(s);
    }
    for (auto& cc : fn.catchClauses()) {
        if (auto cb = cc.body()) {
            for (auto& s : cb->statements()) walkStmtForPromotion(s);
        }
    }
}

void EscapeAnalyzer::walkStmtForPromotion(const ast::Statement& s) {
    if (auto b = s.asBlock()) {
        for (auto& child : b->statements()) walkStmtForPromotion(child);
        return;
    }
    if (auto l = s.asLet()) {
        Symbol* sym = nullptr;
        if (auto* info = analysis.find(l->node.greenNode())) sym = info->resolvedSymbol;
        auto init = l->initializer();
        considerLocalForPromotion(sym, init ? &*init : nullptr);
        return;
    }
    if (auto v = s.asTypedVarDecl()) {
        Symbol* sym = nullptr;
        if (auto* info = analysis.find(v->node.greenNode())) sym = info->resolvedSymbol;
        auto init = v->initializer();
        considerLocalForPromotion(sym, init ? &*init : nullptr);
        return;
    }
    if (auto i = s.asIf()) {
        if (auto t = i->thenBlock()) {
            for (auto& child : t->statements()) walkStmtForPromotion(child);
        }
        if (auto ec = i->elseClause()) {
            if (auto innerIf = ec->ifStatement()) {
                ast::Statement asStmt{innerIf->node};
                walkStmtForPromotion(asStmt);
            } else if (auto bb = ec->block()) {
                for (auto& child : bb->statements()) walkStmtForPromotion(child);
            }
        }
        return;
    }
    if (auto w = s.asWhile()) {
        if (auto body = w->body()) {
            for (auto& child : body->statements()) walkStmtForPromotion(child);
        }
        return;
    }
    if (auto f = s.asFor()) {
        if (auto init = f->init()) walkStmtForPromotion(*init);
        if (auto body = f->body()) {
            for (auto& child : body->statements()) walkStmtForPromotion(child);
        }
        return;
    }
    if (auto fe = s.asForEach()) {
        if (auto body = fe->body()) {
            for (auto& child : body->statements()) walkStmtForPromotion(child);
        }
        return;
    }
}

void EscapeAnalyzer::considerLocalForPromotion(Symbol* sym, const ast::Expression* init) {
    if (!sym) return;
    if (sym->kind != SymbolKind::Variable) return;
    if (!sym->type || !sym->type->isArray()) return;
    if (!sym->type->inner) return;
    if (sym->localEscape != EscapeKind::NoEscape) return;
    if (sym->reassigned) return;
    if (!init) return;
    if (!initIsStackPromotable(*init)) return;
    sym->stackPromoted = true;
}

bool EscapeAnalyzer::initIsStackPromotable(const ast::Expression& init) const {
    if (auto n = init.asNew()) {
        if (!n->isArrayNew()) return false;
        auto sizes = n->arraySizeExpressions();
        if (sizes.size() != 1) return false;
        auto lit = sizes[0].asLiteral();
        if (!lit) return false;
        SyntaxKind k = lit->literalKind();
        return k == SyntaxKind::IntLiteral || k == SyntaxKind::LongLiteral;
    }
    if (init.asArrayLiteral()) {
        return true;
    }
    if (auto p = init.asParen()) {
        if (auto inner = p->inner()) return initIsStackPromotable(*inner);
    }
    return false;
}
