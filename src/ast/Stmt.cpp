#include "Stmt.h"

static std::string asciiOf(std::u16string_view s) {
    std::string r;
    r.reserve(s.size());
    for (char16_t c : s) r.push_back(c < 128 ? static_cast<char>(c) : '?');
    return r;
}

void BlockStmt::dump(std::ostream& os, int indent) const {
    writeIndent(os, indent);
    os << "Block\n";
    for (const auto& s : statements) s->dump(os, indent + 1);
}

void VarDeclStmt::dump(std::ostream& os, int indent) const {
    writeIndent(os, indent);
    os << "VarDecl(" << asciiOf(name) << ")\n";
    if (type) {
        writeIndent(os, indent + 1);
        os << "type:\n";
        type->dump(os, indent + 2);
    }
    if (init) {
        writeIndent(os, indent + 1);
        os << "init:\n";
        init->dump(os, indent + 2);
    }
}

void ExprStmt::dump(std::ostream& os, int indent) const {
    writeIndent(os, indent);
    os << "ExprStmt\n";
    expr->dump(os, indent + 1);
}

void ReturnStmt::dump(std::ostream& os, int indent) const {
    writeIndent(os, indent);
    os << "Return\n";
    if (expr) expr->dump(os, indent + 1);
}

void IfStmt::dump(std::ostream& os, int indent) const {
    writeIndent(os, indent);
    os << "If\n";
    writeIndent(os, indent + 1);
    os << "cond:\n";
    condition->dump(os, indent + 2);
    writeIndent(os, indent + 1);
    os << "then:\n";
    thenBranch->dump(os, indent + 2);
    if (elseBranch) {
        writeIndent(os, indent + 1);
        os << "else:\n";
        elseBranch->dump(os, indent + 2);
    }
}

void WhileStmt::dump(std::ostream& os, int indent) const {
    writeIndent(os, indent);
    os << "While\n";
    writeIndent(os, indent + 1);
    os << "cond:\n";
    condition->dump(os, indent + 2);
    writeIndent(os, indent + 1);
    os << "body:\n";
    body->dump(os, indent + 2);
}

static const char* visName(Visibility v) {
    switch (v) {
        case Visibility::Public:    return "public";
        case Visibility::Private:   return "private";
        case Visibility::Protected: return "protected";
    }
    return "?";
}

void StructDecl::dump(std::ostream& os, int indent) const {
    writeIndent(os, indent);
    os << "StructDecl(" << asciiOf(name) << ")\n";
    for (const auto& f : fields) {
        writeIndent(os, indent + 1);
        os << asciiOf(f.name) << ":\n";
        if (f.type) f.type->dump(os, indent + 2);
    }
    for (const auto& m : methods) {
        m->dump(os, indent + 1);
    }
}

void FuncDecl::dump(std::ostream& os, int indent) const {
    writeIndent(os, indent);
    os << "FuncDecl(" << visName(visibility) << " " << asciiOf(name) << ")\n";
    if (!parameters.empty()) {
        writeIndent(os, indent + 1);
        os << "params:\n";
        for (const auto& p : parameters) {
            writeIndent(os, indent + 2);
            os << asciiOf(p.name) << ":\n";
            p.type->dump(os, indent + 3);
        }
    }
    if (returnType) {
        writeIndent(os, indent + 1);
        os << "returns:\n";
        returnType->dump(os, indent + 2);
    }
    writeIndent(os, indent + 1);
    os << "body:\n";
    body->dump(os, indent + 2);
}
