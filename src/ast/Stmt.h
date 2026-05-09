#pragma once
#include <memory>
#include <string>
#include <vector>
#include "Node.h"
#include "Expr.h"
#include "Type.h"

class Symbol;

class Stmt : public Node {};
using StmtPtr = std::unique_ptr<Stmt>;

enum class Visibility { Public, Private, Protected };

struct Parameter {
    TypePtr type;
    std::u16string name;
    Symbol* resolvedSymbol = nullptr;
};

class BlockStmt : public Stmt {
public:
    std::vector<StmtPtr> statements;
    void dump(std::ostream& os, int indent) const override;
};

class VarDeclStmt : public Stmt {
public:
    TypePtr type;
    std::u16string name;
    ExprPtr init;
    Symbol* resolvedSymbol = nullptr;

    VarDeclStmt(TypePtr t, std::u16string n, ExprPtr i)
        : type(std::move(t)), name(std::move(n)), init(std::move(i)) {}
    void dump(std::ostream& os, int indent) const override;
};

class ExprStmt : public Stmt {
public:
    ExprPtr expr;
    explicit ExprStmt(ExprPtr e) : expr(std::move(e)) {}
    void dump(std::ostream& os, int indent) const override;
};

class ReturnStmt : public Stmt {
public:
    ExprPtr expr;
    explicit ReturnStmt(ExprPtr e) : expr(std::move(e)) {}
    void dump(std::ostream& os, int indent) const override;
};

class IfStmt : public Stmt {
public:
    ExprPtr condition;
    StmtPtr thenBranch;
    StmtPtr elseBranch;

    IfStmt(ExprPtr c, StmtPtr t, StmtPtr e)
        : condition(std::move(c)), thenBranch(std::move(t)), elseBranch(std::move(e)) {}
    void dump(std::ostream& os, int indent) const override;
};

class WhileStmt : public Stmt {
public:
    ExprPtr condition;
    StmtPtr body;

    WhileStmt(ExprPtr c, StmtPtr b)
        : condition(std::move(c)), body(std::move(b)) {}
    void dump(std::ostream& os, int indent) const override;
};

class FuncDecl : public Stmt {
public:
    Visibility visibility = Visibility::Public;
    std::u16string name;
    std::vector<Parameter> parameters;
    TypePtr returnType;
    std::unique_ptr<BlockStmt> body;
    Symbol* resolvedSymbol = nullptr;

    // Set when this FuncDecl is a method on a struct/class. The owning type's
    // semantic Type. The synthetic `this` parameter is always the first one
    // and is added by the analyzer.
    ::Type* receiverType = nullptr;
    Symbol* thisSymbol = nullptr;  // synthetic `this` parameter symbol for methods

    void dump(std::ostream& os, int indent) const override;
};

struct StructField {
    TypePtr type;
    std::u16string name;
    Visibility visibility = Visibility::Public;
    int line = 0;
    int column = 0;
};

class Type;  // forward — separate from ast::TypeNode

class StructDecl : public Stmt {
public:
    Visibility visibility = Visibility::Public;
    std::u16string name;
    std::vector<StructField> fields;
    std::vector<std::unique_ptr<FuncDecl>> methods;
    ::Type* resolvedType = nullptr;  // canonical semantic type

    void dump(std::ostream& os, int indent) const override;
};
