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

    ~Analyzer();

    // Single-file convenience wrapper: collect → bind imports → analyze bodies,
    // all in one call.
    void analyze(const SyntaxNode& sourceFileRoot);

    // Multi-file pipeline: each driver step runs once per module before the
    // next step starts.
    void collectDeclarations(const SyntaxNode& sourceFileRoot);
    void bindImports(const ModuleResolver& resolver);
    void analyzeBodies();

    void importPrelude();

    StructInfo* errorClass() const { return errorClassInfo_; }

    const AnalysisResult& result() const { return analysis; }
    AnalysisResult& result() { return analysis; }

    TypeContext& types() { return typeCtx; }
    const std::u16string& modulePath() const { return modulePath_; }

    // Used by other Analyzers' bindImports to look up an exported symbol in
    // this module. Returns nullptr if no such symbol exists at the global
    // scope. Functions live as Symbols; structs/classes are surfaced via a
    // synthetic Variable-kind symbol whose `type` is the user-defined type.
    Symbol* globalSymbol(const std::u16string& name) const;

    // Distinct library names referenced by `external from "..."` blocks in
    // this module. Order matches first occurrence in source.
    const std::vector<std::u16string>& linkLibraries() const { return linkLibraries_; }

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
    std::vector<std::u16string> linkLibraries_;
    Scope* globalScope = nullptr;
    Scope* currentScope = nullptr;
    Symbol* currentFunction = nullptr;
    Symbol* currentThis = nullptr;
    bool sawSuperConstructorCall = false;
    // The function's parameter/this scope; catch clauses and the body are children
    // of it, so catch clauses see params/this but not body locals.
    Scope* currentFunctionParamScope = nullptr;
    bool inCatchClause = false;

    // Cached AST root after collectDeclarations so analyzeBodies doesn't have
    // to re-parse the source. Populated by collectDeclarations.
    std::optional<ast::SourceFile> astRoot;

    StructInfo* errorClassInfo_ = nullptr;

    // In owning-TypeContext (single-file/LSP) mode the analyzer compiles the
    // prelude into its own context here, so `Error` resolves without a driver.
    // Empty in shared-context mode (the driver supplies a prelude module).
    struct PreludeData;
    std::unique_ptr<PreludeData> prelude_;
    void bootstrapPrelude();

    Symbol* makeSymbol(SymbolKind k, std::u16string n, Type* t, uint32_t offset);
    Scope* pushScope();
    void popScope();
    void registerBuiltins();

    void error(uint32_t offset, int length, std::string message);
    void errorAtNode(const SyntaxNode& node, std::string message);

    // === Collect phase ===
    void registerStructNames(const ast::SourceFile& file);
    void registerClassNames(const ast::SourceFile& file);
    void registerExternalTypeNames(const ast::SourceFile& file);
    void collectStructs(const ast::SourceFile& file);
    void collectClasses(const ast::SourceFile& file);
    void collectFunctions(const ast::SourceFile& file);
    void collectExternalFunctions(const ast::SourceFile& file);
    void resolveMethodParams(const ast::FuncDecl& fn, ::Type* receiverType, Symbol* sym);
    void resolveFunctionParams(const ast::FuncDecl& fn, Symbol* sym);
    void checkThrowsClausePlacement(const ast::FuncDecl& fn, bool isOverridable, bool isConstructor);
    void checkFieldMethodCollision(StructInfo* owner, const std::u16string& methodName,
                                   bool isConstructor, const SyntaxNode& diag);
    void resolveDeclaredThrows(const ast::FuncDecl& fn, Symbol* sym);
    void checkParameterDefaults(const ast::FuncDecl& fn);
    void checkFieldDefaults(const ast::StructDecl& sd);
    void checkFieldDefaults(const ast::ClassDecl& cd);
    void checkFieldInitialization(const ast::StructDecl& sd);
    void checkFieldInitialization(const ast::ClassDecl& cd);

    // === Body analysis ===
    void analyzeFunctionBody(const ast::FuncDecl& fn);
    void analyzeImplicitConstructorAssignments(const ast::FuncDecl& fn);
    void analyzeCatchClause(const ast::CatchClause& clause, Scope* funcScope);

    void analyzeStatement(const ast::Statement& stmt);
    void analyzeBlock(const ast::Block& block);
    void analyzeLetStmt(const ast::LetStatement& stmt);
    void analyzeTypedVarDeclStmt(const ast::TypedVarDeclStatement& stmt);
    void analyzeIfStmt(const ast::IfStatement& stmt);
    void analyzeWhileStmt(const ast::WhileStatement& stmt);
    void analyzeReturnStmt(const ast::ReturnStatement& stmt);
    void analyzeExpressionStmt(const ast::ExpressionStatement& stmt);
    void analyzeThrowStmt(const ast::ThrowStatement& stmt);
    void analyzeRethrowStmt(const ast::RethrowStatement& stmt);

    Type* analyzeExpr(const ast::Expression& expr);
    Type* analyzeLiteral(const ast::LiteralExpression& expr);
    Type* analyzeIdent(const ast::IdentExpression& expr);
    Type* analyzeThis(const ast::ThisExpression& expr);
    Type* analyzeSuper(const ast::SuperExpression& expr);
    Type* analyzeBinary(const ast::BinaryExpression& expr);
    Type* analyzePrefix(const ast::PrefixExpression& expr);
    Type* analyzeCall(const ast::CallExpression& expr);
    Type* analyzeExternalCall(const ast::CallExpression& expr, Symbol* sym,
                              const std::u16string& funcName);
    Type* analyzeMember(const ast::MemberExpression& expr);
    Type* analyzeSafeMember(const ast::SafeMemberExpression& expr);
    Type* analyzeSubscript(const ast::SubscriptExpression& expr);
    Type* analyzeSafeSubscript(const ast::SafeSubscriptExpression& expr);
    Type* analyzeCast(const ast::CastExpression& expr);

    void tryAdaptIntegerLiteral(const ast::Expression& src, Type* target);
    void tryAdaptCharLiteral(const ast::Expression& src, Type* target);
    Type* numericCommonType(Type* a, Type* b);
    // analyzeExpr + try to adapt an integer literal to `target` in one step.
    // Returns the (possibly retyped) expression type.
    Type* analyzeExprAdapt(const ast::Expression& expr, Type* target);
    Type* analyzeAssign(const ast::AssignExpression& expr);
    Type* analyzeTernary(const ast::TernaryExpression& expr);
    Type* analyzeNew(const ast::NewExpression& expr);
    Type* analyzeTry(const ast::TryExpression& expr);
    Type* analyzeParen(const ast::ParenExpression& expr);
    Type* analyzeArrayLiteral(const ast::ArrayLiteralExpression& expr);
    Type* analyzeArrayLiteralAdapt(const ast::ArrayLiteralExpression& expr, Type* target);

    // Build a NarrowingPath from a member / subscript chain. Returns nullopt when
    // any segment is something we can't reliably re-recognize on later reads
    // (calls, arithmetic indices, etc.).
    std::optional<NarrowingPath> buildNarrowingPath(
        const ast::Expression& expr,
        std::vector<Symbol*>* indexSymbols = nullptr) const;

    void clearNarrowingsForCall(const ast::CallExpression& expr);

    struct NullCheckInfo {
        NarrowingPath key;
        Type* narrowedT = nullptr;
        bool narrowsThen = false;
        bool valid = false;
    };
    NullCheckInfo detectNullCheck(const ast::Expression& cond);
    void analyzeBranchWithNarrowing(const ast::Block& block,
                                    const NullCheckInfo& info, bool installNarrowing);

    Type* resolveTypeReference(const ast::TypeReference& tr);
    Type* lookupTypeByName(const std::u16string& qualifier, const std::u16string& name,
                           const SyntaxNode& diagNode);
    bool isLValue(const ast::Expression& expr) const;

    bool isMemberAccessAllowed(Visibility visibility, StructInfo* definingClass);
    bool isLocalClass(StructInfo* definingClass);
    void checkMemberAccess(const SyntaxNode& diagNode, const std::u16string& memberName,
                           Visibility visibility, StructInfo* definingClass);

    // True if `t` can be considered non-null without an initializer.
    bool isDefaultable(Type* t) const;
    bool validateArrayElement(Type* elem, const SyntaxNode& diagNode);

    // Helpers for CST → location.
    int lineOf(uint32_t offset) const;
    int columnOf(uint32_t offset) const;
};
