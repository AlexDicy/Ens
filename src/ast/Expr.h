#pragma once
#include <memory>
#include <string>
#include <vector>
#include "Node.h"
#include "../tokenizer/TokenType.h"

class Type;
class Symbol;

class Expr : public Node {
public:
    Type* resolvedType = nullptr;
};
using ExprPtr = std::unique_ptr<Expr>;

class IntLitExpr : public Expr {
public:
    long long value;
    explicit IntLitExpr(long long v) : value(v) {}
    void dump(std::ostream& os, int indent) const override;
};

class DoubleLitExpr : public Expr {
public:
    double value;
    explicit DoubleLitExpr(double v) : value(v) {}
    void dump(std::ostream& os, int indent) const override;
};

class StringLitExpr : public Expr {
public:
    std::u16string value;
    explicit StringLitExpr(std::u16string v) : value(std::move(v)) {}
    void dump(std::ostream& os, int indent) const override;
};

class BoolLitExpr : public Expr {
public:
    bool value;
    explicit BoolLitExpr(bool v) : value(v) {}
    void dump(std::ostream& os, int indent) const override;
};

class NullLitExpr : public Expr {
public:
    void dump(std::ostream& os, int indent) const override;
};

class IdentExpr : public Expr {
public:
    std::u16string name;
    Symbol* resolvedSymbol = nullptr;
    explicit IdentExpr(std::u16string n) : name(std::move(n)) {}
    void dump(std::ostream& os, int indent) const override;
};

class BinaryExpr : public Expr {
public:
    TokenType op;
    ExprPtr left;
    ExprPtr right;
    BinaryExpr(TokenType op, ExprPtr l, ExprPtr r)
        : op(op), left(std::move(l)), right(std::move(r)) {}
    void dump(std::ostream& os, int indent) const override;
};

class UnaryExpr : public Expr {
public:
    TokenType op;
    ExprPtr operand;
    UnaryExpr(TokenType op, ExprPtr o) : op(op), operand(std::move(o)) {}
    void dump(std::ostream& os, int indent) const override;
};

class CallExpr : public Expr {
public:
    ExprPtr callee;
    std::vector<ExprPtr> args;
    CallExpr(ExprPtr c, std::vector<ExprPtr> a)
        : callee(std::move(c)), args(std::move(a)) {}
    void dump(std::ostream& os, int indent) const override;
};

class MemberExpr : public Expr {
public:
    ExprPtr object;
    std::u16string member;
    MemberExpr(ExprPtr o, std::u16string m)
        : object(std::move(o)), member(std::move(m)) {}
    void dump(std::ostream& os, int indent) const override;
};

class SubscriptExpr : public Expr {
public:
    ExprPtr object;
    ExprPtr index;
    SubscriptExpr(ExprPtr o, ExprPtr i)
        : object(std::move(o)), index(std::move(i)) {}
    void dump(std::ostream& os, int indent) const override;
};

class AssignExpr : public Expr {
public:
    TokenType op;
    ExprPtr target;
    ExprPtr value;
    AssignExpr(TokenType op, ExprPtr t, ExprPtr v)
        : op(op), target(std::move(t)), value(std::move(v)) {}
    void dump(std::ostream& os, int indent) const override;
};

class TernaryExpr : public Expr {
public:
    ExprPtr cond;
    ExprPtr thenExpr;
    ExprPtr elseExpr;
    TernaryExpr(ExprPtr c, ExprPtr t, ExprPtr e)
        : cond(std::move(c)), thenExpr(std::move(t)), elseExpr(std::move(e)) {}
    void dump(std::ostream& os, int indent) const override;
};
