#include "ThrowsAnalyzer.h"

#include <algorithm>
#include "../diagnostics/DiagnosticSink.h"
#include "../diagnostics/SourceFile.h"
#include "Symbol.h"
#include "Type.h"

namespace {

std::string asciiOf(std::u16string_view s) {
    std::string r;
    r.reserve(s.size());
    for (char16_t c : s) r.push_back(c < 128 ? static_cast<char>(c) : '?');
    return r;
}

StructInfo* structOfType(Type* t) {
    return (t && t->isClass()) ? t->structInfo : nullptr;
}

std::string callDisplayName(const ast::CallExpression& call) {
    auto callee = call.callee();
    if (!callee) return "<call>";
    if (auto id = callee->asIdent()) return asciiOf(id->nameText().value_or(std::u16string{}));
    if (auto m = callee->asMember()) return asciiOf(m->memberText().value_or(std::u16string{}));
    if (auto sm = callee->asSafeMember()) return asciiOf(sm->memberText().value_or(std::u16string{}));
    if (callee->asSuper()) return "super";
    return "<call>";
}

bool subtreeHasRethrow(const SyntaxNode& node) {
    if (node.kind() == SyntaxKind::RethrowStmt) return true;
    for (auto& c : node.children()) {
        if (subtreeHasRethrow(c)) return true;
    }
    return false;
}

}  // namespace

ThrowsAnalyzer::ThrowsAnalyzer(const ast::SourceFile& sourceFile, const AnalysisResult& a,
                               StructInfo* errorClassInfo)
    : sf(sourceFile), analysis(a), errorClass(errorClassInfo) {}

bool ThrowsAnalyzer::addType(TypeSet& set, StructInfo* t) {
    if (!t) return false;
    auto it = std::lower_bound(set.begin(), set.end(), t);
    if (it != set.end() && *it == t) return false;
    set.insert(it, t);
    return true;
}

bool ThrowsAnalyzer::contains(const TypeSet& set, StructInfo* t) {
    return std::binary_search(set.begin(), set.end(), t);
}

bool ThrowsAnalyzer::covers(const TypeSet& set, StructInfo* m) {
    if (!m) return true;
    for (StructInfo* d : set) {
        if (m->isSubclassOf(d)) return true;
    }
    return false;
}

Symbol* ThrowsAnalyzer::calleeSymbolOf(const ast::CallExpression& call) const {
    auto callee = call.callee();
    if (!callee) return nullptr;
    if (auto id = callee->asIdent()) {
        auto* info = analysis.find(id->node.greenNode());
        return info ? info->resolvedSymbol : nullptr;
    }
    if (auto m = callee->asMember()) {
        auto* info = analysis.find(m->node.greenNode());
        if (!info) return nullptr;
        // Methods resolve to resolvedMethodSymbol; a namespace-qualified free-function
        // call (ns.func) stores the function in resolvedSymbol instead.
        return info->resolvedMethodSymbol ? info->resolvedMethodSymbol : info->resolvedSymbol;
    }
    if (auto sm = callee->asSafeMember()) {
        auto* info = analysis.find(sm->node.greenNode());
        if (!info) return nullptr;
        return info->resolvedMethodSymbol ? info->resolvedMethodSymbol : info->resolvedSymbol;
    }
    // super(...) calls a base constructor, which can never propagate.
    return nullptr;
}

bool ThrowsAnalyzer::isOpaqueCall(const ast::CallExpression& call) const {
    if (Symbol* sym = calleeSymbolOf(call); sym && sym->throwsOpaquely) return true;
    auto callee = call.callee();
    if (!callee) return false;
    Type* calleeType = analysis.typeOf(callee->node.greenNode());
    while (calleeType && calleeType->isOptional()) calleeType = calleeType->inner;
    return calleeType && calleeType->isFunction() && calleeType->hasThrowsClause;
}

bool ThrowsAnalyzer::hasOpaqueTriedCall(const SyntaxNode& node, bool triedOperand) const {
    SyntaxKind k = node.kind();
    if (k == SyntaxKind::LambdaExpr) return false;
    if (k == SyntaxKind::TryExpr) {
        for (auto& c : node.children()) {
            if (ast::Expression::cast(c) && hasOpaqueTriedCall(c, /*triedOperand=*/true)) return true;
        }
        return false;
    }
    if (k == SyntaxKind::CallExpr) {
        if (triedOperand) {
            if (auto call = ast::CallExpression::cast(node); call && isOpaqueCall(*call)) return true;
        }
        for (auto& c : node.children()) if (hasOpaqueTriedCall(c, false)) return true;
        return false;
    }
    for (auto& c : node.children()) if (hasOpaqueTriedCall(c, triedOperand)) return true;
    return false;
}

const ThrowsAnalyzer::TypeSet& ThrowsAnalyzer::contractOf(const Symbol* sym) const {
    static const TypeSet kEmpty;
    if (!sym) return kEmpty;
    if (!sym->declaredThrowsTypes.empty()) return sym->declaredThrowsTypes;
    // A generic instance's method clone may have been copied before the template
    // resolved its declared list; the template's list is authoritative (declared
    // throws types never mention type parameters).
    if (sym->declaredThrows && sym->methodOwner && sym->methodOwner->templateOf) {
        StructInfo* owner = sym->methodOwner;
        StructInfo* templ = owner->templateOf;
        for (size_t i = 0; i < owner->methods.size() && i < templ->methods.size(); ++i) {
            if (owner->methods[i].symbol != sym) continue;
            Symbol* ts = templ->methods[i].symbol;
            if (ts && !ts->declaredThrowsTypes.empty()) return ts->declaredThrowsTypes;
            break;
        }
    }
    return sym->throwsSet;
}

void ThrowsAnalyzer::collectBlockThrows(const SyntaxNode& node, TypeSet& out) const {
    SyntaxKind k = node.kind();
    // A lambda's body is not part of the function that writes it: a function type never
    // throws, so what its body can raise answers to the lambda, not to the enclosing
    // contract. This front end draws no conclusion from it either way.
    if (k == SyntaxKind::LambdaExpr) return;
    if (k == SyntaxKind::ThrowStmt) {
        if (auto th = ast::ThrowStatement::cast(node)) {
            if (auto v = th->value()) addType(out, structOfType(analysis.typeOf(v->node.greenNode())));
        }
    } else if (k == SyntaxKind::CallExpr) {
        if (auto call = ast::CallExpression::cast(node)) {
            for (StructInfo* m : contractOf(calleeSymbolOf(*call))) addType(out, m);
        }
    }
    for (auto& c : node.children()) collectBlockThrows(c, out);
}

ThrowsAnalyzer::TypeSet ThrowsAnalyzer::bodySetOf(const ast::FuncDecl& fn) const {
    TypeSet body;
    if (auto b = fn.body()) collectBlockThrows(b->node, body);
    return body;
}

ThrowsAnalyzer::TypeSet ThrowsAnalyzer::computeOutward(const ast::FuncDecl& fn) const {
    TypeSet body = bodySetOf(fn);
    auto clauses = fn.catchClauses();

    std::vector<StructInfo*> clauseTypes;
    clauseTypes.reserve(clauses.size());
    for (auto& cc : clauses) {
        StructInfo* ct = nullptr;
        if (auto tr = cc.typeReference()) ct = structOfType(analysis.typeOf(tr->node.greenNode()));
        clauseTypes.push_back(ct);
    }

    auto caughtByEarlier = [&](StructInfo* m, size_t upto) {
        for (size_t j = 0; j < upto; ++j) {
            if (clauseTypes[j] && m->isSubclassOf(clauseTypes[j])) return true;
        }
        return false;
    };

    TypeSet outward;
    // Uncaught body members. A member fully caught by some clause is removed; a
    // member that is only a strict superclass of a clause type stays (it might
    // not match that clause at runtime).
    for (StructInfo* m : body) {
        if (!caughtByEarlier(m, clauseTypes.size())) addType(outward, m);
    }

    // rethrow re-raises what its clause caught; throws/calls in a catch body escape.
    for (size_t i = 0; i < clauses.size(); ++i) {
        StructInfo* c = clauseTypes[i];
        if (auto cb = clauses[i].body()) {
            if (c && subtreeHasRethrow(clauses[i].body()->node)) {
                bool partial = false;
                for (StructInfo* m : body) {
                    if (caughtByEarlier(m, i)) continue;
                    if (m->isSubclassOf(c)) addType(outward, m);          // fully caught here
                    else if (c->isSubclassOf(m)) partial = true;          // c is a subclass of m
                }
                if (partial) addType(outward, c);
            }
            collectBlockThrows(cb->node, outward);
        }
    }
    return outward;
}

void ThrowsAnalyzer::runOnceForFunction(Symbol* sym, const ast::FuncDecl& fn) {
    TypeSet outward = computeOutward(fn);
    for (StructInfo* m : outward) {
        if (addType(sym->throwsSet, m)) changed = true;
    }
    // A try on an opaque call may raise types this pass cannot enumerate; its catch
    // clauses may or may not handle them, so the enclosing function is opaque too.
    if (!sym->throwsOpaquely) {
        if (auto b = fn.body(); b && hasOpaqueTriedCall(b->node)) {
            sym->throwsOpaquely = true;
            changed = true;
        }
    }
}

// A test body has no catch clauses; everything it throws goes outward, and the
// implicit declared contract (`throws Error`) covers all of it.
void ThrowsAnalyzer::runOnceForTest(Symbol* sym, const ast::TestDecl& td) {
    TypeSet outward;
    if (auto b = td.body()) collectBlockThrows(b->node, outward);
    for (StructInfo* m : outward) {
        if (addType(sym->throwsSet, m)) changed = true;
    }
}

bool ThrowsAnalyzer::runOnce() {
    changed = false;
    for (auto& fn : sf.functions()) {
        auto* info = analysis.find(fn.node.greenNode());
        if (info && info->resolvedSymbol) runOnceForFunction(info->resolvedSymbol, fn);
    }
    for (auto& td : sf.tests()) {
        auto* info = analysis.find(td.node.greenNode());
        if (info && info->resolvedSymbol) runOnceForTest(info->resolvedSymbol, td);
    }
    for (auto& sd : sf.structs()) {
        for (auto& m : sd.methods()) {
            auto* info = analysis.find(m.node.greenNode());
            if (info && info->resolvedSymbol) runOnceForFunction(info->resolvedSymbol, m);
        }
    }
    for (auto& cd : sf.classes()) {
        for (auto& m : cd.methods()) {
            auto* info = analysis.find(m.node.greenNode());
            if (info && info->resolvedSymbol) runOnceForFunction(info->resolvedSymbol, m);
        }
    }
    return changed;
}

// =========================================================
// Validation
// =========================================================

void ThrowsAnalyzer::errorAt(const SyntaxNode& node, const std::string& message) {
    auto [offset, length] = node.contentRange();
    auto [line, column] = source_->offsetToPosition(offset);
    sink_->error({line, column, length > 0 ? static_cast<int>(length) : 1}, message);
}

std::string ThrowsAnalyzer::nameList(const TypeSet& set) {
    std::vector<std::string> names;
    names.reserve(set.size());
    for (StructInfo* s : set) names.push_back(asciiOf(s->name));
    std::sort(names.begin(), names.end());
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) out += ", ";
        out += "'" + names[i] + "'";
    }
    return out;
}

void ThrowsAnalyzer::validateTryUsage(const SyntaxNode& node, bool triedOperand) {
    SyntaxKind k = node.kind();
    if (k == SyntaxKind::TryExpr) {
        for (auto& c : node.children()) {
            if (ast::Expression::cast(c)) validateTryUsage(c, /*triedOperand=*/true);
        }
        return;
    }
    if (k == SyntaxKind::CallExpr) {
        if (auto call = ast::CallExpression::cast(node)) {
            const TypeSet& contract = contractOf(calleeSymbolOf(*call));
            std::string name = callDisplayName(*call);
            if (!contract.empty() && !triedOperand) {
                errorAt(node, "Call to '" + name + "' can throw " + nameList(contract) +
                    "; prefix it with 'try' (and handle it with a 'catch' clause or mark the "
                    "function 'throws').");
            } else if (contract.empty() && triedOperand && !isOpaqueCall(*call)) {
                errorAt(node, "'try' is not needed here: '" + name +
                    "' cannot throw. Remove 'try'.");
            }
        }
        for (auto& c : node.children()) validateTryUsage(c, false);
        return;
    }
    for (auto& c : node.children()) validateTryUsage(c, false);
}

void ThrowsAnalyzer::validateNoThrowingCalls(const SyntaxNode& node, const char* contextDescription) {
    if (node.kind() == SyntaxKind::CallExpr) {
        if (auto call = ast::CallExpression::cast(node)) {
            const TypeSet& contract = contractOf(calleeSymbolOf(*call));
            if (!contract.empty()) {
                errorAt(node, std::string(contextDescription) + " cannot call '" +
                    callDisplayName(*call) + "' because it can throw " + nameList(contract) +
                    "; it must not throw.");
            }
        }
    }
    for (auto& c : node.children()) validateNoThrowingCalls(c, contextDescription);
}

void ThrowsAnalyzer::validateFunction(Symbol* sym, const ast::FuncDecl& fn, bool isConstructor,
                                      StructInfo* receiver) {
    const TypeSet& outward = sym->throwsSet;
    const TypeSet& declared = sym->declaredThrowsTypes;
    TypeSet body = bodySetOf(fn);
    auto clauses = fn.catchClauses();

    // Try usage across the body and each catch body.
    if (auto b = fn.body()) validateTryUsage(b->node, false);
    for (auto& cc : clauses) {
        if (auto cb = cc.body()) validateTryUsage(cb->node, false);
    }

    // Parameter defaults must not throw (no enclosing handler).
    for (auto& p : fn.parameters()) {
        if (auto dv = p.defaultValue()) {
            if (auto e = dv->expression()) validateNoThrowingCalls(e->node, "A default value");
        }
    }

    if (isConstructor) {
        if (!outward.empty()) {
            errorAt(fn.nameToken().value_or(fn.node), "Constructor '" + asciiOf(sym->name) +
                "' cannot let exceptions escape, but it can throw " + nameList(outward) +
                ". Catch it with a 'catch' clause after the constructor body.");
        }
    } else if (sym->isDestructor) {
        if (!outward.empty()) {
            std::string owner = receiver ? " of '" + asciiOf(receiver->name) + "'" : "";
            errorAt(fn.nameToken().value_or(fn.node), "Destructor" + owner +
                " cannot let exceptions escape, but it can throw " + nameList(outward) +
                ". Catch the exceptions with 'catch' clauses after the destructor body.");
        }
    } else if (sym->declaredThrows) {
        if (!declared.empty()) {
            // Computed outward must stay within the declared contract.
            TypeSet stray;
            for (StructInfo* m : outward) if (!covers(declared, m)) addType(stray, m);
            if (!stray.empty()) {
                errorAt(fn.nameToken().value_or(fn.node), "'" + asciiOf(sym->name) +
                    "' can throw " + nameList(stray) +
                    ", which is not in its declared throws list. Add it to the list, or handle it "
                    "with 'catch' clauses.");
            }
        } else if (outward.empty() && !fn.isAbstract() && !sym->throwsOpaquely) {
            errorAt(fn.throwsToken().value_or(fn.node), "'" + asciiOf(sym->name) +
                "' is marked 'throws' but cannot throw; remove 'throws'.");
        }
    } else if (!outward.empty()) {
        errorAt(fn.nameToken().value_or(fn.node), "'" + asciiOf(sym->name) +
            "' can throw " + nameList(outward) +
            " but is not marked 'throws'. Add 'throws' after the signature, or handle the "
            "exceptions with 'catch' clauses after the body.");
    }

    // Overrides may only narrow the overridden method's contract.
    if (receiver && receiver->baseInfo && !isConstructor) {
        StructInfo* baseDecl = receiver->baseInfo->classDeclaringMethodBySignature(sym->name, sym);
        if (baseDecl) {
            int bi = baseDecl->findMethodIndexBySignature(sym->name, sym);
            Symbol* baseSym = bi >= 0 ? baseDecl->methods[bi].symbol : nullptr;
            if (baseSym) {
                const TypeSet& mine = contractOf(sym);
                const TypeSet& base = contractOf(baseSym);
                TypeSet stray;
                for (StructInfo* m : mine) if (!covers(base, m)) addType(stray, m);
                if (!stray.empty()) {
                    errorAt(fn.nameToken().value_or(fn.node), "Override '" + asciiOf(sym->name) +
                        "' can throw " + nameList(stray) + ", which '" + asciiOf(baseDecl->name) +
                        "' does not allow. An override may only throw types already allowed by "
                        "the method it overrides.");
                }
            }
        }
    }

    // Catch-clause reachability.
    if (!clauses.empty()) {
        std::vector<StructInfo*> clauseTypes;
        for (auto& cc : clauses) {
            StructInfo* ct = nullptr;
            if (auto tr = cc.typeReference()) ct = structOfType(analysis.typeOf(tr->node.greenNode()));
            clauseTypes.push_back(ct);
        }
        if (body.empty() && !sym->throwsOpaquely) {
            errorAt(clauses[0].node, "'" + asciiOf(sym->name) +
                "' has 'catch' clauses but its body cannot throw; remove them.");
        }
        for (size_t i = 0; i < clauses.size(); ++i) {
            StructInfo* ci = clauseTypes[i];
            if (!ci) continue;
            bool shadowed = false;
            for (size_t j = 0; j < i; ++j) {
                if (clauseTypes[j] && ci->isSubclassOf(clauseTypes[j])) { shadowed = true; break; }
            }
            if (shadowed) {
                errorAt(clauses[i].node, "This 'catch (" + asciiOf(ci->name) +
                    " ...)' is unreachable: an earlier clause already handles it. Move it before "
                    "the broader clause, or remove it.");
                continue;
            }
            // An opaque body may throw types this pass cannot enumerate, so it cannot
            // rule out a clause as dead.
            if (body.empty() || sym->throwsOpaquely) continue;
            bool live = false;
            for (StructInfo* m : body) {
                if (m->isSubclassOf(ci) || ci->isSubclassOf(m)) { live = true; break; }
            }
            if (!live) {
                errorAt(clauses[i].node, "This 'catch (" + asciiOf(ci->name) +
                    " ...)' is dead: the body of '" + asciiOf(sym->name) +
                    "' cannot throw " + asciiOf(ci->name) + " or a subclass. Remove the clause.");
            }
        }
    }
}

void ThrowsAnalyzer::validate(DiagnosticSink& sink, const SourceFile& source) {
    sink_ = &sink;
    source_ = &source;

    for (auto& fn : sf.functions()) {
        auto* info = analysis.find(fn.node.greenNode());
        if (info && info->resolvedSymbol) validateFunction(info->resolvedSymbol, fn, false, nullptr);
    }
    // Test bodies get the same try-usage checks; their declared contract
    // (`throws Error`) covers any computed set, so no contract check is needed.
    for (auto& td : sf.tests()) {
        auto* info = analysis.find(td.node.greenNode());
        if (!info || !info->resolvedSymbol) continue;
        if (auto b = td.body()) validateTryUsage(b->node, false);
    }
    auto eachMethod = [&](const ast::FuncDecl& m) {
        auto* info = analysis.find(m.node.greenNode());
        if (!info || !info->resolvedSymbol) return;
        Type* recv = analysis.receiverOf(m.node.greenNode());
        StructInfo* ri = recv ? recv->structInfo : nullptr;
        bool isCtor = info->resolvedSymbol->isConstructor;
        validateFunction(info->resolvedSymbol, m, isCtor, ri);
    };
    for (auto& sd : sf.structs()) for (auto& m : sd.methods()) eachMethod(m);
    for (auto& cd : sf.classes()) for (auto& m : cd.methods()) eachMethod(m);

    // A method satisfying an interface may only throw what the interface method
    // declares (subclasses of a declared type included).
    for (auto& cd : sf.classes()) {
        Type* t = analysis.typeOf(cd.node.greenNode());
        StructInfo* si = (t && t->isClass()) ? t->structInfo : nullptr;
        if (!si) continue;
        for (Type* ifaceT : si->implementedInterfaces) {
            StructInfo* iface = (ifaceT && ifaceT->isClass()) ? ifaceT->structInfo : nullptr;
            if (!iface) continue;
            for (auto& im : iface->methods) {
                if (!im.symbol) continue;
                StructInfo* decl = si->classDeclaringMethodBySignature(im.name, im.symbol);
                if (!decl) continue;  // missing method reported during class layout
                int idx = decl->findMethodIndexBySignature(im.name, im.symbol);
                Symbol* implSym = idx >= 0 ? decl->methods[idx].symbol : nullptr;
                if (!implSym || implSym == im.symbol) continue;
                const TypeSet& mine = contractOf(implSym);
                const TypeSet& allowed = contractOf(im.symbol);
                TypeSet stray;
                for (StructInfo* m : mine) if (!covers(allowed, m)) addType(stray, m);
                if (!stray.empty()) {
                    errorAt(cd.node, "Method '" + asciiOf(im.name) + "' of '" +
                        asciiOf(si->name) + "' can throw " + nameList(stray) +
                        ", which interface '" + ifaceT->toString() + "' does not allow. An "
                        "implementing method may only throw types the interface method declares.");
                }
            }
        }
    }

    // Field defaults must not throw.
    auto checkFieldDefaults = [&](auto fields) {
        for (auto& f : fields) {
            if (auto dv = f.defaultValue()) {
                if (auto e = dv->expression()) validateNoThrowingCalls(e->node, "A field default");
            }
        }
    };
    for (auto& sd : sf.structs()) checkFieldDefaults(sd.fields());
    for (auto& cd : sf.classes()) checkFieldDefaults(cd.fields());

    sink_ = nullptr;
    source_ = nullptr;
}
