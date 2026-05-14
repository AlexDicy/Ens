#pragma once
#include <functional>
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
class Analyzer;

using ModuleResolver = std::function<const Analyzer*(const std::u16string& modulePath)>;

class Analyzer {
public:
    // Owning-TypeContext form: used by the LSP and stdin compilation, where
    // there is exactly one source file and no cross-file resolution.
    Analyzer(const SourceFile& source, DiagnosticSink& sink);

    // Shared-TypeContext form: used by the driver to compile a multi-file
    // program. `modulePath` is the canonical key (e.g. u"engine.renderer") used
    // when registering / looking up types in the shared context.
    Analyzer(const SourceFile& source, DiagnosticSink& sink,
             TypeContext& sharedContext, std::u16string modulePath);

    // Single-file convenience wrapper: collect → bind imports → analyze bodies,
    // all in one call.
    void analyze(const SyntaxNode& sourceFileRoot);

    // Multi-file pipeline: each driver step runs once per module before the
    // next step starts.
    void collectDeclarations(const SyntaxNode& sourceFileRoot);
    void bindImports(const ModuleResolver& resolver);
    void analyzeBodies();

    const AnalysisResult& result() const { return analysis; }
    AnalysisResult& result() { return analysis; }

    TypeContext& types() { return typeCtx; }
    const std::u16string& modulePath() const { return modulePath_; }

    // Used by other Analyzers' bindImports to look up an exported symbol in
    // this module. Returns nullptr if no such symbol exists at the global
    // scope. Functions live as Symbols; structs/classes are surfaced via a
    // synthetic Variable-kind symbol whose `type` is the user-defined type.
    Symbol* globalSymbol(const std::u16string& name) const;

private:
    const SourceFile& source;
    DiagnosticSink& sink;
    AnalysisResult analysis;

    // When constructed in single-file mode the analyzer owns its TypeContext
    // and `typeCtx` aliases it. In shared-context mode `typeCtx` aliases the
    // driver-owned context and `ownedTypeCtx` stays empty.
    std::unique_ptr<TypeContext> ownedTypeCtx;
    TypeContext& typeCtx;
    std::u16string modulePath_;

    std::vector<std::unique_ptr<Symbol>> ownedSymbols;
    std::vector<std::unique_ptr<Scope>> ownedScopes;
    Scope* globalScope = nullptr;
    Scope* currentScope = nullptr;
    Symbol* currentFunction = nullptr;
    Symbol* currentThis = nullptr;

    // Cached AST root after collectDeclarations so analyzeBodies doesn't have
    // to re-parse the source. Populated by collectDeclarations.
    std::optional<ast::SourceFile> astRoot;

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
    void checkFieldDefaults(const ast::StructDecl& sd);
    void checkFieldDefaults(const ast::ClassDecl& cd);

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
    Type* lookupTypeByName(const std::u16string& qualifier, const std::u16string& name,
                           const SyntaxNode& diagNode);
    bool isLValue(const ast::Expression& expr) const;

    // Helpers for CST → location.
    int lineOf(uint32_t offset) const;
    int columnOf(uint32_t offset) const;
};
