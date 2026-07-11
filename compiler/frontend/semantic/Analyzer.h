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
    // next step starts. `collectDeclarations` = registerNames + resolveSignatures;
    // the driver splits them so imports are bound between the two, letting
    // signatures, field types, and base classes name imported types.
    void collectDeclarations(const SyntaxNode& sourceFileRoot);
    void registerNames(const SyntaxNode& sourceFileRoot);
    void resolveSignatures();
    void bindImports(const ModuleResolver& resolver);
    void bindTypeImports(const ModuleResolver& resolver);
    void bindValueImports(const ModuleResolver& resolver);
    void analyzeBodies();

    void importPrelude();

    void layoutOneClass(const ast::ClassDecl& classDecl);
    static void finalizeClassHierarchy(const std::vector<StructInfo*>& classes);

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
    // Nesting depth of enclosing loops in the current function, for break/continue.
    int loopDepth = 0;

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
    void registerInterfaceNames(const ast::SourceFile& file);
    void registerEnumNames(const ast::SourceFile& file);
    void registerExternalTypeNames(const ast::SourceFile& file);
    void collectStructs(const ast::SourceFile& file);
    void collectInterfaces(const ast::SourceFile& file);
    void collectEnums(const ast::SourceFile& file);
    void resolveClassBases(const ast::SourceFile& file);
    void layoutDeclaredClasses(const ast::SourceFile& file);
    void collectFunctions(const ast::SourceFile& file);
    void collectTests(const ast::SourceFile& file);
    void collectExternalFunctions(const ast::SourceFile& file);
    void resolveMethodParams(const ast::FuncDecl& fn, ::Type* receiverType, Symbol* sym,
                             bool isInterfaceMethod = false);
    void resolveFunctionParams(const ast::FuncDecl& fn, Symbol* sym);
    bool overrideSignaturesCompatible(const MethodInfo& base, Symbol* derived);
    void checkThrowsClausePlacement(const ast::FuncDecl& fn, bool isOverridable, bool isConstructor);
    void checkFieldMethodCollision(StructInfo* owner, const std::u16string& methodName,
                                   bool isConstructor, const SyntaxNode& diag);
    void checkHashMethodSignature(const ast::FuncDecl& fn, Symbol* sym, bool isConstructor);
    void resolveDeclaredThrows(const ast::FuncDecl& fn, Symbol* sym);
    void checkParameterDefaults(const ast::FuncDecl& fn);
    void checkFieldDefaults(const ast::StructDecl& sd);
    void checkFieldDefaults(const ast::ClassDecl& cd);
    void checkFieldInitialization(const ast::StructDecl& sd);
    void checkFieldInitialization(const ast::ClassDecl& cd);

    // === Body analysis ===
    void analyzeFunctionBody(const ast::FuncDecl& fn);
    void analyzeTestBody(const ast::TestDecl& test);
    void analyzeImplicitConstructorAssignments(const ast::FuncDecl& fn);
    void analyzeCatchClause(const ast::CatchClause& clause, Scope* funcScope);

    void analyzeStatement(const ast::Statement& stmt);
    void analyzeBlock(const ast::Block& block);
    void analyzeLetStmt(const ast::LetStatement& stmt);
    void analyzeTypedVarDeclStmt(const ast::TypedVarDeclStatement& stmt);
    void analyzeIfStmt(const ast::IfStatement& stmt);
    void analyzeWhileStmt(const ast::WhileStatement& stmt);
    void analyzeForStmt(const ast::ForStatement& stmt);
    void analyzeForEachStmt(const ast::ForEachStatement& stmt);
    ::Type* resolveIterableElement(::Type* iterT, const SyntaxNode& diag);
    void analyzeBreakStmt(const ast::BreakStatement& stmt);
    void analyzeContinueStmt(const ast::ContinueStatement& stmt);
    void analyzeReturnStmt(const ast::ReturnStatement& stmt);
    void analyzeExpressionStmt(const ast::ExpressionStatement& stmt);
    void analyzeThrowStmt(const ast::ThrowStatement& stmt);
    void analyzeRethrowStmt(const ast::RethrowStatement& stmt);
    void analyzeSwitchStmt(const ast::SwitchStatement& stmt);
    Type* analyzeSwitchExpr(const ast::SwitchExpression& expr);
    Type* analyzeSwitchArms(const std::optional<ast::Expression>& scrutinee,
                            const std::vector<ast::SwitchArm>& arms,
                            const SyntaxNode& diagNode, bool requireValue);

    // === Overload resolution ===
    struct NamedArgInfo {
        std::u16string name;
        ast::Expression value;
        SyntaxNode node;        // the NamedArgument node, for diagnostics
        Type* type = nullptr;
    };
    struct CallShape {
        std::vector<ast::Expression> positional;
        std::vector<Type*> positionalTypes;
        std::vector<NamedArgInfo> named;
        bool malformed = false;   // bad named-argument usage, already reported
        bool hasErrorArg = false; // an argument failed to type; suppress selection noise
    };
    struct OverloadCandidate {
        Symbol* symbol = nullptr;
        const MethodInfo* method = nullptr;  // null for free functions
        bool accessible = true;
    };
    struct OverloadChoice {
        Symbol* symbol = nullptr;
        const MethodInfo* method = nullptr;
        std::vector<int> argParamIndex;      // source-order argument -> parameter index
        bool accessible = true;
        bool failed = false;                 // resolution error already reported
    };
    CallShape analyzeCallShape(const std::vector<ast::Expression>& args);
    bool mapCallArguments(Symbol* sym, const CallShape& shape, std::vector<int>& mapping,
                          int& defaultedCount, std::string& failure,
                          const std::string& kindWord, const std::string& displayName);
    OverloadChoice resolveOverloadedCall(const std::vector<OverloadCandidate>& candidates,
                                         const CallShape& shape, const SyntaxNode& diagNode,
                                         const std::string& displayName,
                                         const std::string& kindWord);
    Type* checkResolvedCallArguments(const CallShape& shape, const OverloadChoice& choice,
                                     const GreenElement* callNode);
    StructInfo* receiverStructInfo(const std::optional<ast::Expression>& obj, bool unwrapOptional);

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
    Type* checkDirectCallArguments(const ast::CallExpression& expr, Symbol* sym,
                                   const std::u16string& funcName);
    Type* analyzeGenericCall(const ast::CallExpression& expr, Symbol* sym,
                             const std::u16string& funcName);
    Type* analyzeMember(const ast::MemberExpression& expr);
    Type* analyzeSafeMember(const ast::SafeMemberExpression& expr);
    Type* analyzeSubscript(const ast::SubscriptExpression& expr);
    Type* analyzeSafeSubscript(const ast::SafeSubscriptExpression& expr);
    Type* analyzeCast(const ast::CastExpression& expr);
    Type* analyzeCheckedCast(const ast::CheckedCastExpression& expr);
    Type* analyzeTypeTest(const ast::TypeTestExpression& expr);
    // Shared checks for 'is' and 'as?': class-only target, class or nullable
    // class scrutinee, related hierarchies. Returns false after reporting.
    bool checkClassTypeTest(Type* srcT, Type* dstT, bool isCast,
                            const SyntaxNode& diagNode);
    // The target of a switch type arm must be a class and a strict subclass of
    // the scrutinee's class `inner`. Returns its StructInfo, or null after
    // reporting.
    StructInfo* checkTypeArmTarget(Type* scrutType, Type* inner, Type* armT,
                                   const SyntaxNode& diagNode);

    void tryAdaptIntegerLiteral(const ast::Expression& src, Type* target);
    void tryAdaptCharLiteral(const ast::Expression& src, Type* target);
    Type* numericCommonType(Type* a, Type* b);
    // analyzeExpr + try to adapt an integer literal to `target` in one step.
    // Returns the (possibly retyped) expression type.
    Type* analyzeExprAdapt(const ast::Expression& expr, Type* target);
    Type* analyzeAssign(const ast::AssignExpression& expr);
    Type* analyzeTernary(const ast::TernaryExpression& expr);
    Type* analyzeNullCoalesce(const ast::NullCoalesceExpression& expr);
    Type* analyzeNew(const ast::NewExpression& expr);
    Type* analyzeTry(const ast::TryExpression& expr);
    Type* analyzeParen(const ast::ParenExpression& expr);
    Type* analyzeArrayLiteral(const ast::ArrayLiteralExpression& expr);
    Type* analyzeArrayLiteralAdapt(const ast::ArrayLiteralExpression& expr, Type* target);
    Type* analyzeInterpString(const ast::InterpStringExpression& expr);

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
    NullCheckInfo detectTypeTest(const ast::TypeTestExpression& test);
    // All null checks the condition establishes when it evaluates to
    // `conditionHolds`: descends `&&` when the condition holds and `||` when it
    // does not, so every conjunct's narrowing is collected.
    void collectNarrowings(const ast::Expression& cond, bool conditionHolds,
                           std::vector<NullCheckInfo>& out);
    void analyzeBranchWithNarrowing(const ast::Block& block,
                                    const std::vector<NullCheckInfo>& narrowings);

    Type* resolveTypeReference(const ast::TypeReference& tr);
    Type* lookupTypeByName(const std::u16string& qualifier, const std::u16string& name,
                           const SyntaxNode& diagNode);

    // Generics. typeParamScope_ maps type-parameter names visible in the enclosing
    // template (class/struct/function) to their placeholder types.
    std::vector<std::pair<std::u16string, Type*>> typeParamScope_;
    std::vector<std::vector<StructInfo*>> resolveTypeParamBounds(
        const void* owner, const std::vector<ast::TypeParam>& params);
    size_t pushTypeParams(const void* owner, const std::vector<std::u16string>& names,
                          const std::vector<std::vector<StructInfo*>>& bounds);
    // Resolve a template's bounds (once) from its AST params, then push its scope.
    size_t enterTemplateScope(StructInfo* si, const std::vector<ast::TypeParam>& astParams);
    void popTypeParams(size_t count);
    // Instantiate `templateType` for the type args written on `tr`, checking arity
    // and bounds; reports against `diag`. Returns the error type on failure.
    Type* instantiateFromArgs(Type* templateType, const std::vector<ast::TypeReference>& args,
                              const SyntaxNode& diag);
    bool checkTypeArgBound(Type* arg, const std::vector<StructInfo*>& bounds,
                           const std::u16string& paramName, const SyntaxNode& diag);
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
