#pragma once
#include <memory>
#include <unordered_map>
#include <vector>
#include "../diagnostics/SourceFile.h"
#include "Scope.h"
#include "Symbol.h"
#include "Type.h"
#include "TypeContext.h"
#include "../cst/SyntaxNode.h"
#include "../ast/Declaration.h"
#include "../ast/Expression.h"
#include "../ast/Statement.h"
#include "../ast/TypeReference.h"
#include "AnalysisResult.h"

class DiagnosticSink;

class Analyzer {
public:
    Analyzer(const SourceFile& source, DiagnosticSink& sink);

    void analyze(const SyntaxNode& sourceFileRoot);

    const AnalysisResult& result() const { return analysis; }
    AnalysisResult& result() { return analysis; }

    TypeContext& types() { return typeCtx; }

private:
    const SourceFile& source;
    DiagnosticSink& sink;
    AnalysisResult analysis;

    TypeContext typeCtx;
    std::vector<std::unique_ptr<Symbol>> ownedSymbols;
    std::vector<std::unique_ptr<Scope>> ownedScopes;
    Scope* globalScope = nullptr;
    Scope* currentScope = nullptr;
    Symbol* currentFunction = nullptr;
    Symbol* currentThis = nullptr;

    Symbol* makeSymbol(SymbolKind k, std::u16string n, Type* t, uint32_t offset);
    Scope* pushScope();
    void popScope();
    void registerBuiltins();

    void error(uint32_t offset, int length, std::string message);
    void errorAtNode(const SyntaxNode& node, std::string message);

    // === Collect phase ===
    void collectStructs(const ast::SourceFile& file);
    void collectClasses(const ast::SourceFile& file);
    void collectFunctions(const ast::SourceFile& file);
    void resolveMethodParams(const ast::FuncDecl& fn, ::Type* receiverType, Symbol* sym);
    void resolveFunctionParams(const ast::FuncDecl& fn, Symbol* sym);
    void checkParameterDefaults(const ast::FuncDecl& fn);

    // === Body analysis ===
    void analyzeFunctionBody(const ast::FuncDecl& fn);
    void analyzeImplicitConstructorAssignments(const ast::FuncDecl& fn);

    void analyzeStatement(const ast::Statement& stmt);
    void analyzeBlock(const ast::Block& block);
    void analyzeLetStmt(const ast::LetStatement& stmt);
    void analyzeTypedVarDeclStmt(const ast::TypedVarDeclStatement& stmt);
    void analyzeIfStmt(const ast::IfStatement& stmt);
    void analyzeWhileStmt(const ast::WhileStatement& stmt);
    void analyzeReturnStmt(const ast::ReturnStatement& stmt);
    void analyzeExpressionStmt(const ast::ExpressionStatement& stmt);

    Type* analyzeExpr(const ast::Expression& expr);
    Type* analyzeLiteral(const ast::LiteralExpression& expr);
    Type* analyzeIdent(const ast::IdentExpression& expr);
    Type* analyzeThis(const ast::ThisExpression& expr);
    Type* analyzeBinary(const ast::BinaryExpression& expr);
    Type* analyzePrefix(const ast::PrefixExpression& expr);
    Type* analyzeCall(const ast::CallExpression& expr);
    Type* analyzeMember(const ast::MemberExpression& expr);
    Type* analyzeAssign(const ast::AssignExpression& expr);
    Type* analyzeTernary(const ast::TernaryExpression& expr);
    Type* analyzeNew(const ast::NewExpression& expr);
    Type* analyzeParen(const ast::ParenExpression& expr);

    Type* resolveTypeReference(const ast::TypeReference& tr);
    bool isLValue(const ast::Expression& expr) const;

    // Helpers for CST → location.
    int lineOf(uint32_t offset) const;
    int columnOf(uint32_t offset) const;
};
