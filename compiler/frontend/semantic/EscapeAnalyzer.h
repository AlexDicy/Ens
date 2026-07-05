#pragma once

#include "../ast/Declaration.h"
#include "../ast/Expression.h"
#include "../ast/Statement.h"
#include "AnalysisResult.h"
#include "Symbol.h"

class EscapeAnalyzer {
public:
    EscapeAnalyzer(const ast::SourceFile& sourceFile, const AnalysisResult& analysis);

    // Single pass over the source file's functions. Returns true if any escape
    // fact changed. Caller drives the fixpoint (allows cross-module convergence
    // when multiple modules' facts depend on each other).
    bool runOnce();

    // Convergent fixpoint for single-module use.
    void analyze() { while (runOnce()) {} finalize(); decideStackPromotions(); }

    // Walk function bodies once to record the textually-last read reference for each class-typed local.
    void finalize();

    void decideStackPromotions();

private:
    const ast::SourceFile& sf;
    const AnalysisResult& analysis;

    Symbol* currentFn = nullptr;
    std::vector<Symbol*> currentParams;
    bool changedThisIteration = false;
    int loopDepth = 0;

    void analyzeFunction(Symbol* fnSym, const ast::FuncDecl& fn);
    void analyzeTest(Symbol* fnSym, const ast::TestDecl& td);
    void collectFunctionsOnce();

    void scanStatement(const ast::Statement& s);
    void scanBlock(const ast::Block& b);
    void scanLetStmt(const ast::LetStatement& s);
    void scanTypedVarDecl(const ast::TypedVarDeclStatement& s);
    void scanIf(const ast::IfStatement& s);
    void scanWhile(const ast::WhileStatement& s);
    void scanFor(const ast::ForStatement& s);
    void scanForEach(const ast::ForEachStatement& s);
    void scanReturn(const ast::ReturnStatement& s);
    void scanExprStmt(const ast::ExpressionStatement& s);

    void scanExpression(const ast::Expression& e);
    void scanIdent(const ast::IdentExpression& e);
    void scanAssign(const ast::AssignExpression& e);
    void scanCall(const ast::CallExpression& e);
    void scanMember(const ast::MemberExpression& e);
    void scanSubscript(const ast::SubscriptExpression& e);
    void scanBinary(const ast::BinaryExpression& e);
    void scanTernary(const ast::TernaryExpression& e);
    void scanSwitchArms(const std::optional<ast::Expression>& scrutinee,
                        const std::vector<ast::SwitchArm>& arms);
    void scanParen(const ast::ParenExpression& e);
    void scanNew(const ast::NewExpression& e);
    void scanArrayLiteral(const ast::ArrayLiteralExpression& e);

    int paramIndexOfSymbol(Symbol* sym) const;
    Symbol* aliasRoot(Symbol* sym) const;

    void markSymbolEscape(Symbol* sym);
    void markSymbolReassigned(Symbol* sym);
    void markParamMutated(int paramIdx);

    // Mark the underlying symbol referenced by `e` (alias-aware) as Escape.
    void markEscapeIfRef(const ast::Expression& e);

    bool isParameterBorrowSource(const ast::Expression& e) const;
    void updateBorrowMode(Symbol* target, const ast::Expression& rhs);
    bool isBorrowModeSymbol(Symbol* sym) const;

    void walkBodyForLastUses(const ast::FuncDecl& fn);
    void walkStmtForLastUses(const ast::Statement& s);
    void walkExprForLastUses(const ast::Expression& e);
    void recordRead(const ast::IdentExpression& id);

    void walkBodyForPromotion(const ast::FuncDecl& fn);
    void walkStmtForPromotion(const ast::Statement& s);
    void considerLocalForPromotion(Symbol* sym, const ast::Expression* init);
    bool initIsStackPromotable(const ast::Expression& init) const;
};
