#include "Expr.h"

static std::string asciiOf(std::u16string_view s) {
    std::string r;
    r.reserve(s.size());
    for (char16_t c : s) r.push_back(c < 128 ? static_cast<char>(c) : '?');
    return r;
}

static std::string opName(TokenType op) {
    return asciiOf(getTokenName(op));
}

void IntLitExpr::dump(std::ostream& os, int indent) const {
    writeIndent(os, indent);
    os << "IntLit(" << value << ")\n";
}

void DoubleLitExpr::dump(std::ostream& os, int indent) const {
    writeIndent(os, indent);
    os << "DoubleLit(" << value << ")\n";
}

void StringLitExpr::dump(std::ostream& os, int indent) const {
    writeIndent(os, indent);
    os << "StringLit(\"" << asciiOf(value) << "\")\n";
}

void BoolLitExpr::dump(std::ostream& os, int indent) const {
    writeIndent(os, indent);
    os << "BoolLit(" << (value ? "true" : "false") << ")\n";
}

void NullLitExpr::dump(std::ostream& os, int indent) const {
    writeIndent(os, indent);
    os << "NullLit\n";
}

void IdentExpr::dump(std::ostream& os, int indent) const {
    writeIndent(os, indent);
    os << "Ident(" << asciiOf(name) << ")\n";
}

void BinaryExpr::dump(std::ostream& os, int indent) const {
    writeIndent(os, indent);
    os << "Binary(" << opName(op) << ")\n";
    left->dump(os, indent + 1);
    right->dump(os, indent + 1);
}

void UnaryExpr::dump(std::ostream& os, int indent) const {
    writeIndent(os, indent);
    os << "Unary(" << opName(op) << ")\n";
    operand->dump(os, indent + 1);
}

void CallExpr::dump(std::ostream& os, int indent) const {
    writeIndent(os, indent);
    os << "Call\n";
    writeIndent(os, indent + 1);
    os << "callee:\n";
    callee->dump(os, indent + 2);
    writeIndent(os, indent + 1);
    os << "args:\n";
    for (const auto& a : args) a->dump(os, indent + 2);
}

void MemberExpr::dump(std::ostream& os, int indent) const {
    writeIndent(os, indent);
    os << "Member(." << asciiOf(member) << ")\n";
    object->dump(os, indent + 1);
}

void SubscriptExpr::dump(std::ostream& os, int indent) const {
    writeIndent(os, indent);
    os << "Subscript\n";
    writeIndent(os, indent + 1);
    os << "object:\n";
    object->dump(os, indent + 2);
    writeIndent(os, indent + 1);
    os << "index:\n";
    index->dump(os, indent + 2);
}

void AssignExpr::dump(std::ostream& os, int indent) const {
    writeIndent(os, indent);
    os << "Assign(" << opName(op) << ")\n";
    writeIndent(os, indent + 1);
    os << "target:\n";
    target->dump(os, indent + 2);
    writeIndent(os, indent + 1);
    os << "value:\n";
    value->dump(os, indent + 2);
}
