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
    if (auto r = s.asReturn()) { scanReturn(*r); return; }
    if (auto e = s.asExpressionStmt()) { scanExprStmt(*e); return; }
}

void EscapeAnalyzer::scanBlock(const ast::Block& b) {
    for (auto& child : b.statements()) {
        scanStatement(child);
    }
}

void EscapeAnalyzer::scanLetStmt(const ast::LetStatement& s) {
    if (auto init = s.initializer()) {
        markEscapeIfParamRef(*init);
        scanExpression(*init);
    }
}

void EscapeAnalyzer::scanTypedVarDecl(const ast::TypedVarDeclStatement& s) {
    if (auto init = s.initializer()) {
        markEscapeIfParamRef(*init);
        scanExpression(*init);
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

void EscapeAnalyzer::scanReturn(const ast::ReturnStatement& s) {
    if (auto v = s.value()) {
        markEscapeIfParamRef(*v);
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
    if (auto bn = e.asBinary()) { scanBinary(*bn); return; }
    if (auto t = e.asTernary()) { scanTernary(*t); return; }
    if (auto p = e.asParen()) { scanParen(*p); return; }
    if (auto n = e.asNew()) { scanNew(*n); return; }
    if (auto id = e.asIdent()) { scanIdent(*id); return; }
}

void EscapeAnalyzer::scanIdent(const ast::IdentExpression&) {}

void EscapeAnalyzer::scanAssign(const ast::AssignExpression& e) {
    auto target = e.target();
    auto value = e.value();
    if (!target || !value) return;

    if (auto m = target->asMember()) {
        // Mutation detection: walk member chain to find the root.
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
                int idx = paramIndexOfSymbol(s);
                if (idx >= 0) markMutated(idx);
            }
        }
        markEscapeIfParamRef(*value);
    } else if (target->asIdent()) {
        markEscapeIfParamRef(*value);
    }

    scanExpression(*target);
    scanExpression(*value);
}

void EscapeAnalyzer::scanCall(const ast::CallExpression& e) {
    auto callee = e.callee();
    Symbol* calleeSym = nullptr;
    if (callee) {
        if (auto member = callee->asMember()) {
            auto* info = analysis.find(member->node.greenNode());
            calleeSym = info ? info->resolvedMethodSymbol : nullptr;
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
            int paramIdx = paramIndexOfSymbol(argSym);
            if (paramIdx >= 0) {
                bool calleeEscapesParam = true;
                if (calleeSym && i < calleeSym->escapeInfo.params.size()) {
                    calleeEscapesParam = (calleeSym->escapeInfo.params[i] == EscapeKind::Escape);
                }
                if (calleeEscapesParam) {
                    markEscape(paramIdx);
                }
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

void EscapeAnalyzer::markEscapeIfParamRef(const ast::Expression& e) {
    if (auto id = e.asIdent()) {
        auto* info = analysis.find(id->node.greenNode());
        Symbol* s = info ? info->resolvedSymbol : nullptr;
        int idx = paramIndexOfSymbol(s);
        if (idx >= 0) markEscape(idx);
        return;
    }
    if (auto p = e.asParen()) {
        if (auto inner = p->inner()) markEscapeIfParamRef(*inner);
        return;
    }
    if (auto t = e.asTernary()) {
        if (auto th = t->thenBranch()) markEscapeIfParamRef(*th);
        if (auto el = t->elseBranch()) markEscapeIfParamRef(*el);
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

void EscapeAnalyzer::markEscape(int paramIdx) {
    if (!currentFn || paramIdx < 0) return;
    auto& ei = currentFn->escapeInfo;
    if (paramIdx >= static_cast<int>(ei.params.size())) return;
    if (ei.params[paramIdx] != EscapeKind::Escape) {
        ei.params[paramIdx] = EscapeKind::Escape;
        changedThisIteration = true;
    }
}

void EscapeAnalyzer::markMutated(int paramIdx) {
    if (!currentFn || paramIdx < 0) return;
    auto& ei = currentFn->escapeInfo;
    if (paramIdx >= static_cast<int>(ei.paramMutated.size())) return;
    if (!ei.paramMutated[paramIdx]) {
        ei.paramMutated[paramIdx] = true;
        changedThisIteration = true;
    }
}

void EscapeAnalyzer::collectFunctionsOnce() {}
