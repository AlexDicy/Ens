#pragma once
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
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
    // when registering / looking up types in the shared context. `packagePrefix`
    // is this module's workspace prefix, prepended to bare imports so a package's
    // sibling imports name the same canonical module as an external `@package` import.
    Analyzer(const SourceFile& source, DiagnosticSink& sink,
             TypeContext& sharedContext, std::u16string modulePath,
             std::u16string packagePrefix = {});

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
    // Reports every struct declared in this module that contains itself by value,
    // possibly through structs from other modules. Runs after all modules' struct
    // fields are resolved and generic instantiations are materialized.
    void checkStructValueCycles();
    // Reports every declaration whose signature mentions a type less visible than
    // the declaration itself. Runs after signatures and class layout are complete.
    void checkSignatureVisibility();
    void analyzeBodies();

    void importPrelude();

    void layoutOneClass(const ast::ClassDecl& classDecl);
    static void finalizeClassHierarchy(const std::vector<StructInfo*>& classes);

    StructInfo* errorClass() const { return errorClassInfo_; }

    const AnalysisResult& result() const { return analysis; }
    AnalysisResult& result() { return analysis; }

    TypeContext& types() { return typeCtx; }
    const std::u16string& modulePath() const { return modulePath_; }
    const std::u16string& packagePrefix() const { return packagePrefix_; }

    // Used by other Analyzers' bindImports to look up an exported symbol in
    // this module. Returns nullptr if no such symbol exists at the global
    // scope. Functions live as Symbols; structs/classes are surfaced via a
    // synthetic Variable-kind symbol whose `type` is the user-defined type.
    Symbol* globalSymbol(const std::u16string& name) const;

    // Native-library policy for this module: when restricted, `external from` may name only
    // the natives declared in the owning package's manifest at `manifestPath`.
    void setNativePolicy(bool restricted, std::vector<std::u16string> declaredNatives,
                         std::string manifestPath) {
        restrictNatives_ = restricted;
        declaredNatives_ = std::move(declaredNatives);
        nativeManifestPath_ = std::move(manifestPath);
    }

    // Distinct library names referenced by `external from` blocks in this module, collected
    // only when no manifest governs it (the names then bind by convention at link time).
    // Order matches first occurrence in source.
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
    std::u16string packagePrefix_;

    // The canonical module path an import refers to. `@package`/`@std` imports carry their
    // full path; a bare import is qualified by this module's package prefix so it resolves
    // to the same module as the equivalent `@package` import from outside.
    std::u16string importTargetPath(const ast::ImportDecl& imp) const;

    std::vector<std::unique_ptr<Symbol>> ownedSymbols;
    std::vector<std::unique_ptr<Scope>> ownedScopes;
    std::vector<std::u16string> linkLibraries_;
    bool restrictNatives_ = false;
    std::vector<std::u16string> declaredNatives_;
    std::string nativeManifestPath_;
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

    // === Definite assignment ===
    // A local variable is readable only where it is definitely assigned on every
    // path reaching the read. This is a forward "must" analysis over the statement
    // tree, with set intersection at each join (if/else, switch arms, loop exit).
    // A terminated path (return/throw/rethrow/panic, and break/continue for the
    // relevant loop) contributes nothing to a join. Structs are variable-level: a
    // struct local must be assigned as a whole before any field access.
    // In a constructor body the same analysis also tracks which of the class's own
    // fields are definitely assigned, so a non-defaultable field is required to be
    // written on every path that leaves the constructor normally.
    struct AssignmentFlow {
        std::unordered_set<Symbol*> assigned;
        std::unordered_set<const FieldInfo*> assignedFields;
        bool terminated = false;
    };
    bool assignmentActive_ = false;
    bool flowTerminated_ = false;
    // Whether the current expression position is evaluated unconditionally, so an
    // assignment found here counts as a definite assignment (false inside the
    // short-circuited operand of &&, ||, ??, ?., ?[ and inside ternary branches).
    bool unconditionalPosition_ = true;
    const void* assignmentTargetGreen_ = nullptr;
    std::unordered_set<Symbol*> assignedLocals_;
    std::unordered_set<Symbol*> trackedLocals_;
    std::vector<std::vector<AssignmentFlow>> breakFlows_;
    // The class whose constructor body is being analyzed, or null outside a
    // constructor. `assignedThisFields_` is the current flow's set of own fields
    // definitely assigned; `ctorSeededThisFields_` is the entry state (`this.field`
    // shorthand parameters), which each catch clause resets to on the exception path.
    StructInfo* ctorFieldClass_ = nullptr;
    std::unordered_set<const FieldInfo*> assignedThisFields_;
    std::unordered_set<const FieldInfo*> ctorSeededThisFields_;
    void trackLocal(Symbol* sym, bool assigned);
    void markAssigned(Symbol* sym);
    void markThisFieldAssigned(const FieldInfo* field);
    void checkDefiniteAssignment(const ast::IdentExpression& expr, Symbol* sym);
    // Reports every own non-defaultable field left unassigned on the path reaching
    // `diag`, and credits them so the same field is not reported twice.
    void checkConstructorFieldsAssigned(const SyntaxNode& diag);
    AssignmentFlow snapshotAssignment() const;
    void restoreAssignment(const AssignmentFlow& flow);
    AssignmentFlow joinAssignment(const std::vector<AssignmentFlow>& flows) const;
    void resetAssignmentFlow();

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
    void rejectTopLevelVariables(const ast::SourceFile& file);
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
    void checkNoreturnPlacement(const ast::FuncDecl& fn, bool isConstructor, bool isDestructor);
    void checkEntrySignature(const ast::FuncDecl& fn, Symbol* sym);
    void checkFieldMethodCollision(StructInfo* owner, const std::u16string& methodName,
                                   bool isConstructor, const SyntaxNode& diag);
    void checkHashMethodSignature(const ast::FuncDecl& fn, Symbol* sym, bool isConstructor);
    void checkEqualsMethodSignature(const ast::FuncDecl& fn, Symbol* sym, bool isConstructor);
    void checkToStringMethodSignature(const ast::FuncDecl& fn, Symbol* sym, bool isConstructor);
    void checkStructOverrideMarker(const ast::FuncDecl& fn, const Type* owner,
                                   const std::u16string& memberName, Symbol* sym,
                                   bool isConstructor, const char* behavior);
    void checkStructAbstractMarker(const ast::FuncDecl& fn, const Type* owner,
                                   const std::u16string& memberName, bool isConstructor);
    void checkHashEqualsPairing(const ast::ClassDecl& cd, StructInfo* si);
    void checkStructEquatable(Type* structT, const SyntaxNode& node);
    bool findNonComparableField(Type* structT, std::vector<StructInfo*>& visiting,
                                std::string& fieldPath, Type*& leaf);
    void checkStructJsonable(Type* structT, const SyntaxNode& node);
    bool findNonJsonableField(Type* structT, std::vector<StructInfo*>& visiting,
                              std::string& fieldPath, Type*& leaf);
    void resolveDeclaredThrows(const ast::FuncDecl& fn, Symbol* sym);
    void checkParameterDefaults(const ast::FuncDecl& fn);
    void checkFieldDefaults(const ast::StructDecl& sd);
    void checkFieldDefaults(const ast::ClassDecl& cd);
    void checkFieldInitialization(const ast::StructDecl& sd);
    void checkFieldInitialization(const ast::ClassDecl& cd);

    // === Body analysis ===
    void analyzeFunctionBody(const ast::FuncDecl& fn);
    void analyzeTestBody(const ast::TestDecl& test);
    void checkFunctionReturnPaths(const ast::FuncDecl& fn);
    // Whether a branch's exit leaves only the function (return/throw/rethrow/panic)
    // or also the enclosing loop (adding break/continue). Return-path analysis uses
    // Function; the surviving-branch narrowing carry uses Branch.
    enum class ExitScope { Function, Branch };
    bool statementTerminates(const ast::Statement& stmt, ExitScope scope) const;
    bool blockTerminates(const ast::Block& block, ExitScope scope) const;
    bool ifStatementTerminates(const ast::IfStatement& stmt, ExitScope scope) const;
    bool switchStatementTerminates(const ast::SwitchStatement& stmt, ExitScope scope) const;
    bool whileStatementTerminates(const ast::WhileStatement& stmt) const;
    bool isNoreturnCall(const ast::Expression& expr) const;
    void analyzeImplicitConstructorAssignments(const ast::FuncDecl& fn);
    void analyzeCatchClause(const ast::CatchClause& clause, Scope* funcScope);

    void analyzeStatement(const ast::Statement& stmt);
    // Analyzes a statement list in order, first recognizing the array fill-loop
    // idiom across consecutive statements (declaration + immediate fill loop).
    void analyzeStatements(const std::vector<ast::Statement>& stmts);
    void noteArrayFillLoop(const std::vector<ast::Statement>& stmts, size_t index);
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
    Type* analyzePostfix(const ast::PostfixExpression& expr);
    // Shared checks for prefix and postfix `++`/`--`: the operand must be an
    // assignable numeric lvalue, and a successful form invalidates narrowings on
    // the mutated target exactly as an assignment to it would. Returns the
    // operand type (the result type for both forms).
    Type* analyzeIncDec(const SyntaxNode& exprNode, const ast::Expression& operand,
                        Type* operandT, bool isIncrement, bool isPrefix);
    // Drops any narrowings recorded for the storage written by `target`, mirroring
    // the invalidation an assignment to `target` performs.
    void invalidateNarrowingsForWrite(const ast::Expression& target);
    // The declared storage type of a narrowable operand (plain binding, `this`,
    // or a member/element path), ignoring any active flow narrowing. Returns
    // null when the operand is not a narrowable storage location.
    Type* declaredBindingType(const ast::Expression& operand) const;
    // Re-types a presence-operator operand from its declared type so that a
    // binding declared optional stays testable even after narrowing proved it
    // non-null. Stamps the operand node with the declared type so codegen loads
    // the optional representation storage always holds; the check is constant.
    Type* presenceOperandType(const ast::Expression& operand, Type* analyzed);
    // Narrows a plain local or parameter of optional declared type to its inner
    // type after a straight-line assignment or initializer whose value is
    // statically non-nullable. No-op for any other binding, target, or value.
    void establishAssignmentNarrowing(Symbol* sym, Type* valueT);
    Type* analyzeCall(const ast::CallExpression& expr);
    Type* analyzeExternalCall(const ast::CallExpression& expr, Symbol* sym,
                              const std::u16string& funcName);
    Type* checkDirectCallArguments(const ast::CallExpression& expr, Symbol* sym,
                                   const std::u16string& funcName);
    static bool isFromCStringIntrinsic(const Symbol* sym);
    Type* checkFromCStringCall(const ast::CallExpression& expr);
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
    void adaptIntegerLiteralLabel(const ast::Expression& src, Type* target);
    void tryAdaptCharLiteral(const ast::Expression& src, Type* target);
    Type* numericCommonType(Type* a, Type* b);
    // Least upper bound of two value types, shared by `?:` branches and switch
    // arms: identical types, a numeric common type, an assignable-either-way
    // pair, a `null`/`T` pair yielding `T?`, or two sibling class values sharing
    // a common ancestor yielding the nearest common base class (nullable when
    // either side is). Returns null when the types have no common type.
    Type* unifyValueTypes(Type* a, Type* b);
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
    Type* analyzeStructLiteral(const ast::StructLiteralExpression& expr);
    Type* analyzeStructLiteralAdapt(const ast::StructLiteralExpression& expr, Type* target);
    Type* analyzeStructConstructorCall(const ast::CallExpression& expr, Type* structType,
                                       const std::u16string& typeName);
    Type* analyzeInterpString(const ast::InterpStringExpression& expr);

    // Build a NarrowingPath from a member / subscript chain. Returns nullopt when
    // any segment is something we can't reliably re-recognize on later reads
    // (calls, arithmetic indices, etc.). With `allowAnyIndex`, such an index
    // becomes an AnyIndex segment instead, so write invalidation can treat it
    // as potentially aliasing every element.
    std::optional<NarrowingPath> buildNarrowingPath(
        const ast::Expression& expr,
        std::vector<Symbol*>* indexSymbols = nullptr,
        bool allowAnyIndex = false,
        bool byName = false) const;

    void clearNarrowingsForCall(const ast::CallExpression& expr);
    void clearNarrowingsForArguments(const std::vector<ast::Expression>& args);
    void clearNarrowingsTouchedBy(const ast::Expression& e);

    // Loop-entry pre-clearing: before a loop body is analyzed, drop every
    // narrowing whose storage the body may write, so a fact killed by a later
    // body write cannot be read stale on a following iteration. The roots are
    // resolved by name against the current scope, since the body nodes are not
    // analyzed yet.
    void preClearLoopBodyWrites(const SyntaxNode& node);
    void preClearWrite(const ast::Expression& target);
    void preClearTouch(const ast::Expression& value);

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

    // The narrowing facts that hold at one program point: the proven type of
    // every storage path visible in the current scope chain, innermost first.
    using NarrowingFacts =
        std::unordered_map<NarrowingPath, Type*, NarrowingPathHash>;
    // A saved copy of the narrowing maps in the current scope chain. The arms of
    // an if/else or switch are analyzed from the same entry state and their
    // end-states joined, so each arm restores this before it runs.
    struct NarrowingSnapshot {
        std::vector<std::pair<Scope*, NarrowingFacts>> layers;
    };
    NarrowingSnapshot captureNarrowings() const;
    void restoreNarrowings(const NarrowingSnapshot& snap);
    NarrowingFacts flattenNarrowings() const;
    // Analyzes a branch block under the facts its condition proves and returns the
    // narrowing facts that hold at its end, before the branch scope is discarded.
    NarrowingFacts analyzeBranchCapturing(const ast::Block& block,
                                          const std::vector<NullCheckInfo>& narrowings);
    NarrowingFacts factsFromNarrowings(const std::vector<NullCheckInfo>& narrowings);
    // The proven type common to two branch end-states for one path: identity, or
    // the wider of two class types related by subtyping, or null when neither
    // contains the other and no common narrowing survives.
    Type* unifyNarrowedTypes(Type* a, Type* b);
    // Installs the intersection of the surviving branch end-states as the
    // narrowing state after a merge: a path stays narrowed only when every branch
    // that falls through narrows it, and an entry fact a branch dropped is dropped.
    void applyNarrowingJoin(const NarrowingFacts& entry,
                            const std::vector<NarrowingFacts>& survivors);
    bool pathRootInScope(const NarrowingPath& path) const;

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
    void checkConstructorAccess(const SyntaxNode& diagNode, StructInfo* owner,
                                const MethodInfo& constructor);

    // === Visibility boundaries ===
    // The effective visibility of a top-level declaration: the written modifier,
    // or private when none is written. Top-level 'protected' is rejected.
    Visibility topLevelVisibility(const std::optional<ast::VisibilityModifier>& modifier,
                                  const std::string& declName);
    // The effective visibility of a type or struct/class member, with over-marking
    // (a member more visible than its containing type) rejected.
    Visibility memberVisibility(const std::optional<ast::VisibilityModifier>& modifier,
                                Visibility defaultVisibility, StructInfo* owner,
                                const std::string& memberKindWord,
                                const std::u16string& memberName);
    // The behavior a member replaces for its type, or null when it is an ordinary
    // member; the phrase names the behavior in a diagnostic.
    const char* builtinBehaviorReplaced(const Type* owner, const std::u16string& memberName,
                                        const Symbol* sym) const;
    // The effective visibility of a member that replaces a built-in behavior: it
    // follows its type, and a marker narrower than the type is rejected.
    Visibility builtinReplacementVisibility(
        const std::optional<ast::VisibilityModifier>& modifier, const Type* owner,
        const std::u16string& memberName, const char* behavior);
    // True when a top-level symbol with the given visibility, declared in the given
    // module and package, is nameable from this module.
    bool isTopLevelVisibleFrom(Visibility v, const std::u16string& declModulePath,
                               const std::u16string& declPackagePrefix) const;
    bool isTypeVisibleFrom(const Type* t) const;
    // "Type 'X' is private to module 'M'; ..." with the modifier that fixes it.
    std::string invisibleSymbolMessage(const std::string& kindWord, const std::u16string& name,
                                       Visibility v, const std::u16string& declModulePath,
                                       const std::u16string& declPackagePrefix) const;
    std::string invisibleTypeMessage(const std::u16string& name, const Type* t) const;
    void checkCallableSignatureVisibility(const ast::FuncDecl& fn, Visibility declVisibility,
                                          const std::string& declPhrase,
                                          StructInfo* protectedOwner = nullptr);
    void checkTypeParamBoundsVisibility(const std::vector<ast::TypeParam>& params,
                                        Visibility declVisibility, const std::string& declPhrase,
                                        StructInfo* protectedOwner = nullptr);
    void checkMentionedType(const ast::TypeReference& tr, Visibility declVisibility,
                            const std::string& declPhrase, const std::string& role,
                            StructInfo* protectedOwner = nullptr);
    void checkMentionedTypeValue(Type* t, const SyntaxNode& diagNode, Visibility declVisibility,
                                 const std::string& declPhrase, const std::string& role,
                                 StructInfo* protectedOwner = nullptr);

    // True if `t` can be considered non-null without an initializer. The visiting
    // set keeps a struct-containment cycle (reported separately) from recursing forever.
    bool isDefaultable(Type* t) const;
    bool isDefaultable(Type* t, std::unordered_set<const StructInfo*>& visiting) const;
    // `fillExampleName`, when set, marks the single-dimension `new T[n]` form
    // where the fill-loop idiom applies; the diagnostic then teaches the idiom
    // using that variable name.
    bool validateArrayElement(Type* elem, const SyntaxNode& diagNode,
                              const std::optional<std::u16string>& fillExampleName = std::nullopt);

    // `new T[n]` allocations proven fully written by the fill loop immediately
    // following their declaration, keyed by the NewExpr green node.
    std::unordered_set<const GreenElement*> fillLoopProvenNews_;
    // Declared variable names of single-dimension array-new initializers, used
    // to word the fill-loop diagnostic with the user's own variable name.
    std::unordered_map<const GreenElement*, std::u16string> arrayNewDeclNames_;

    // Helpers for CST → location.
    int lineOf(uint32_t offset) const;
    int columnOf(uint32_t offset) const;
};
