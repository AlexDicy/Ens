#pragma once

#include <vector>
#include "../ast/Declaration.h"
#include "../ast/Expression.h"
#include "AnalysisResult.h"

class DiagnosticSink;
class SourceFile;
struct StructInfo;
class Symbol;

// Computes each function's outward throw set as a fixpoint over the call graph
// (mirrors EscapeAnalyzer: runOnce returns whether any set grew; the driver
// loops across modules). Sets propagate cross-module via shared Symbol*. After
// convergence, validate() emits the checked-exception diagnostics.
//
// Contract model (no upward union): a method's contract is its declared `throws`
// list if present, otherwise its computed outward set. Overrides may only narrow
// the overridden method's contract; a virtual call uses the static target's
// contract, so a downstream override can never enlarge what a caller must handle.
class ThrowsAnalyzer {
public:
    ThrowsAnalyzer(const ast::SourceFile& sourceFile, const AnalysisResult& analysis,
                   StructInfo* errorClass);

    bool runOnce();
    void analyze() { while (runOnce()) {} }
    void validate(DiagnosticSink& sink, const SourceFile& source);

private:
    const ast::SourceFile& sf;
    const AnalysisResult& analysis;
    StructInfo* errorClass;
    bool changed = false;

    using TypeSet = std::vector<StructInfo*>;

    // Sorted-by-pointer insert; returns true if `t` was newly added.
    static bool addType(TypeSet& set, StructInfo* t);
    static bool contains(const TypeSet& set, StructInfo* t);
    // True if some member of `set` is `m` or a superclass of `m` (i.e. covers m).
    static bool covers(const TypeSet& set, StructInfo* m);

    Symbol* calleeSymbolOf(const ast::CallExpression& call) const;
    const TypeSet& contractOf(const Symbol* sym) const;

    // A call whose thrown types the server cannot enumerate: either its callee is a
    // function-typed value (a parameter, local, or field) whose type carries a `throws`
    // clause, or it names a symbol whose own declared throws list named only type
    // parameters. Such a call may throw, but validation must not guess what.
    bool isOpaqueCall(const ast::CallExpression& call) const;
    // True if some call directly under a `try` in this subtree is opaque (does not
    // descend into a nested lambda body, mirroring collectBlockThrows).
    bool hasOpaqueTriedCall(const SyntaxNode& node, bool triedOperand = false) const;

    // Collect throw-operand types and called-function contracts over a block's
    // subtree (does not descend into sibling catch clauses).
    void collectBlockThrows(const SyntaxNode& blockNode, TypeSet& out) const;

    // The function's outward set: body throws minus those caught, plus rethrow
    // and catch-body contributions.
    TypeSet computeOutward(const ast::FuncDecl& fn) const;
    TypeSet bodySetOf(const ast::FuncDecl& fn) const;

    void runOnceForFunction(Symbol* sym, const ast::FuncDecl& fn);
    void runOnceForTest(Symbol* sym, const ast::TestDecl& td);

    // === validation ===
    DiagnosticSink* sink_ = nullptr;
    const SourceFile* source_ = nullptr;
    void errorAt(const SyntaxNode& node, const std::string& message);
    static std::string nameList(const TypeSet& set);

    void validateFunction(Symbol* sym, const ast::FuncDecl& fn, bool isConstructor,
                          StructInfo* receiver);
    void validateTryUsage(const SyntaxNode& node, bool triedOperand);
    void validateNoThrowingCalls(const SyntaxNode& node, const char* contextDescription);
};