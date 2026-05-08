#pragma once
#include <memory>
#include <vector>
#include "Node.h"
#include "Expr.h"
#include "Type.h"

class Stmt : public Node {};
using StmtPtr = std::unique_ptr<Stmt>;

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
