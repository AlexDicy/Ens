#pragma once
#include <memory>
#include <vector>
#include "Type.h"
#include "TypeContext.h"
#include "Symbol.h"
#include "Scope.h"
#include "../ast/Stmt.h"
#include "../ast/Expr.h"
#include "../ast/Type.h"
#include "../diagnostics/Diagnostic.h"

class Analyzer {
public:
    Analyzer();

    void analyze(const std::vector<StmtPtr>& program);

    bool hasErrors() const { return !diagnostics.empty(); }
    const std::vector<Diagnostic>& getDiagnostics() const { return diagnostics; }

private:
    TypeContext typeCtx;
    std::vector<std::unique_ptr<Symbol>> ownedSymbols;
    std::vector<std::unique_ptr<Scope>> ownedScopes;
    Scope* globalScope = nullptr;
    Scope* currentScope = nullptr;
    Symbol* currentFunction = nullptr;
    Symbol* currentThis = nullptr;

    std::vector<Diagnostic> diagnostics;

    Symbol* makeSymbol(SymbolKind k, std::u16string n, Type* t, int line, int col);
    Scope* pushScope();
    void popScope();
    void registerBuiltins();

    void collectStructs(const std::vector<StmtPtr>& program);
    void collectClasses(const std::vector<StmtPtr>& program);
    void collectFunctions(const std::vector<StmtPtr>& program);
    void analyzeFunctionBody(FuncDecl* fn);

    void analyzeStmt(Stmt* s);
    void analyzeBlock(BlockStmt* s);
    void analyzeVarDecl(VarDeclStmt* s);
    void analyzeIf(IfStmt* s);
    void analyzeWhile(WhileStmt* s);
    void analyzeReturn(ReturnStmt* s);
    void analyzeExprStmt(ExprStmt* s);

    Type* analyzeExpr(Expr* e);
    Type* analyzeIdent(IdentExpr* e);
    Type* analyzeThis(ThisExpr* e);
    Type* analyzeBinary(BinaryExpr* e);
    Type* analyzeUnary(UnaryExpr* e);
    Type* analyzeCall(CallExpr* e);
    Type* analyzeMember(MemberExpr* e);
    Type* analyzeAssign(AssignExpr* e);
    Type* analyzeSubscript(SubscriptExpr* e);
    Type* analyzeTernary(TernaryExpr* e);
    Type* analyzeNew(NewExpr* e);

    Type* resolveTypeNode(TypeNode* node);
    bool isLValue(Expr* e);

    void error(int line, int col, int len, std::string msg);
};
