#include "Parser.h"
#include "../diagnostics/Diagnostic.h"
#include <string>

static constexpr int PREC_UNARY = 13;
static constexpr int PREC_POSTFIX = 14;

Parser::Parser(std::vector<Token> ts) : tokens(std::move(ts)) {
    int line = tokens.empty() ? 1 : tokens.back().getLine();
    int col = tokens.empty() ? 1 : tokens.back().getColumn() + tokens.back().getLength();
    tokens.emplace_back(TokenType::END_OF_FILE, std::u16string{}, line, col);
}

bool Parser::atEnd() const {
    return tokens[pos].getType() == TokenType::END_OF_FILE;
}

const Token& Parser::peek() const {
    return tokens[pos];
}

const Token& Parser::consume() {
    return tokens[pos++];
}

bool Parser::check(TokenType type) const {
    return tokens[pos].getType() == type;
}

bool Parser::match(TokenType type) {
    if (check(type)) {
        pos++;
        return true;
    }
    return false;
}

const Token& Parser::expect(TokenType type, const char* what) {
    if (!check(type)) error(peek(), std::string("Expected ") + what);
    return consume();
}

void Parser::error(const Token& t, const std::string& msg) const {
    SourceSpan span{t.getLine(), t.getColumn(), t.getLength() > 0 ? t.getLength() : 1};
    throw Diagnostic(DiagnosticLevel::Error, span, msg);
}

static bool isAssignmentOp(TokenType t) {
    switch (t) {
        case TokenType::EQ:
        case TokenType::PLUS_EQ:
        case TokenType::SUB_EQ:
        case TokenType::STAR_EQ:
        case TokenType::SLASH_EQ:
        case TokenType::PERCENT_EQ:
        case TokenType::BIT_AND_EQ:
        case TokenType::BIT_OR_EQ:
        case TokenType::CARET_EQ:
        case TokenType::LT_LT_EQ:
        case TokenType::GT_GT_EQ:
        case TokenType::GT_GT_GT_EQ:
            return true;
        default:
            return false;
    }
}

// Ternary `?:` precedence — between assignment (1) and logical OR (3).
static constexpr int PREC_TERNARY = 2;

int Parser::infixPrecedence(TokenType type) const {
    if (isAssignmentOp(type)) return 1;
    switch (type) {
        case TokenType::QUES:      return PREC_TERNARY;
        case TokenType::OR:        return 3;
        case TokenType::AND:       return 4;
        case TokenType::BIT_OR:    return 5;
        case TokenType::CARET:     return 6;
        case TokenType::BIT_AND:   return 7;
        case TokenType::EQ_EQ:
        case TokenType::NOT_EQ:    return 8;
        case TokenType::LT:
        case TokenType::GT:
        case TokenType::LT_EQ:
        case TokenType::GT_EQ:     return 9;
        case TokenType::LT_LT:
        case TokenType::GT_GT:
        case TokenType::GT_GT_GT:  return 10;
        case TokenType::PLUS:
        case TokenType::SUB:       return 11;
        case TokenType::STAR:
        case TokenType::SLASH:
        case TokenType::PERCENT:   return 12;
        case TokenType::DOT:
        case TokenType::L_PAREN:
        case TokenType::L_BRACKET: return PREC_POSTFIX;
        default:                   return 0;
    }
}

bool Parser::isRightAssoc(TokenType type) const {
    return isAssignmentOp(type);
}

static long long parseIntText(std::u16string_view text) {
    std::string s;
    s.reserve(text.size());
    for (char16_t c : text) s.push_back(static_cast<char>(c));
    try {
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
            return std::stoll(s.substr(2), nullptr, 2);
        }
        return std::stoll(s, nullptr, 0);
    } catch (...) {
        return 0;
    }
}

static double parseDoubleText(std::u16string_view text) {
    std::string s;
    s.reserve(text.size());
    for (char16_t c : text) s.push_back(static_cast<char>(c));
    return std::stod(s);
}

ExprPtr Parser::parseExpression() {
    return parsePrecedence(1);
}

bool Parser::isPrimitiveType(TokenType t) const {
    switch (t) {
        case TokenType::BOOL:
        case TokenType::BYTE:
        case TokenType::SHORT:
        case TokenType::USHORT:
        case TokenType::INT:
        case TokenType::UINT:
        case TokenType::LONG:
        case TokenType::ULONG:
        case TokenType::FLOAT:
        case TokenType::DOUBLE:
        case TokenType::DECIMAL:
        case TokenType::CHAR:
        case TokenType::STRING:
        case TokenType::VOID:
            return true;
        default:
            return false;
    }
}

TypePtr Parser::parseType() {
    const Token& tok = peek();
    int line = tok.getLine();
    int column = tok.getColumn();
    std::u16string name;
    if (isPrimitiveType(tok.getType()) || tok.getType() == TokenType::IDENTIFIER) {
        name = tok.getText();
        consume();
    } else {
        error(tok, "Expected type name");
    }
    bool isOptional = match(TokenType::QUES);
    auto t = std::make_unique<TypeNode>(std::move(name), isOptional);
    t->line = line;
    t->column = column;
    return t;
}

std::vector<StmtPtr> Parser::parseProgram() {
    std::vector<StmtPtr> stmts;
    while (!atEnd()) {
        stmts.push_back(parseStatement());
    }
    return stmts;
}

StmtPtr Parser::parseStatement() {
    const Token& tok = peek();

    if (tok.getType() == TokenType::PRIVATE ||
        tok.getType() == TokenType::PROTECTED ||
        tok.getType() == TokenType::PUBLIC) {
        Visibility vis = (tok.getType() == TokenType::PRIVATE)   ? Visibility::Private
                       : (tok.getType() == TokenType::PROTECTED) ? Visibility::Protected
                                                                 : Visibility::Public;
        consume();
        if (check(TokenType::STRUCT)) return parseStructDecl(vis);
        return parseFuncDecl(vis);
    }

    if (tok.getType() == TokenType::STRUCT) {
        return parseStructDecl(Visibility::Public);
    }

    if (tok.getType() == TokenType::IDENTIFIER && looksLikeFuncDecl()) {
        return parseFuncDecl(Visibility::Public);
    }

    switch (tok.getType()) {
        case TokenType::LET:      return parseLet();
        case TokenType::IF:       return parseIf();
        case TokenType::WHILE:    return parseWhile();
        case TokenType::RETURN:   return parseReturn();
        case TokenType::L_BRACE:  return parseBlock();
        default:
            if (isPrimitiveType(tok.getType())) return parseTypedVarDecl();
            if (tok.getType() == TokenType::IDENTIFIER && looksLikeTypedDecl()) {
                return parseTypedVarDecl();
            }
            return parseExprStmt();
    }
}

bool Parser::looksLikeFuncDecl() const {
    if (pos >= tokens.size() || tokens[pos].getType() != TokenType::IDENTIFIER) return false;
    if (pos + 1 >= tokens.size() || tokens[pos + 1].getType() != TokenType::L_PAREN) return false;
    int depth = 1;
    size_t j = pos + 2;
    while (j < tokens.size() && depth > 0) {
        TokenType t = tokens[j].getType();
        if (t == TokenType::L_PAREN) depth++;
        else if (t == TokenType::R_PAREN) depth--;
        j++;
    }
    if (depth != 0 || j >= tokens.size()) return false;
    TokenType after = tokens[j].getType();
    return after == TokenType::ARROW || after == TokenType::L_BRACE;
}

std::unique_ptr<FuncDecl> Parser::parseFuncDecl(Visibility vis) {
    const Token& nameTok = expect(TokenType::IDENTIFIER, "function name");
    expect(TokenType::L_PAREN, "'(' after function name");
    auto fn = std::make_unique<FuncDecl>();
    fn->visibility = vis;
    fn->name = nameTok.getText();
    fn->line = nameTok.getLine();
    fn->column = nameTok.getColumn();
    if (!check(TokenType::R_PAREN)) {
        fn->parameters.push_back(parseParameter());
        while (match(TokenType::COMMA)) {
            fn->parameters.push_back(parseParameter());
        }
    }
    expect(TokenType::R_PAREN, "')' after parameters");
    if (match(TokenType::ARROW)) {
        fn->returnType = parseType();
    }
    fn->body = parseBlock();
    return fn;
}

Parameter Parser::parseParameter() {
    Parameter p;
    p.type = parseType();
    const Token& nameTok = expect(TokenType::IDENTIFIER, "parameter name");
    p.name = nameTok.getText();
    return p;
}

StmtPtr Parser::parseStructDecl(Visibility vis) {
    const Token& kw = expect(TokenType::STRUCT, "'struct'");
    const Token& nameTok = expect(TokenType::IDENTIFIER, "struct name");
    expect(TokenType::L_BRACE, "'{' after struct name");

    auto decl = std::make_unique<StructDecl>();
    decl->visibility = vis;
    decl->name = nameTok.getText();
    decl->line = kw.getLine();
    decl->column = kw.getColumn();

    while (!check(TokenType::R_BRACE) && !atEnd()) {
        Visibility memberVis = Visibility::Public;
        if (check(TokenType::PRIVATE))   { consume(); memberVis = Visibility::Private; }
        else if (check(TokenType::PROTECTED)) { consume(); memberVis = Visibility::Protected; }
        else if (check(TokenType::PUBLIC))    { consume(); memberVis = Visibility::Public; }

        // Method or field?
        // Method: starts with IDENT and has the func-decl shape (`name(...) [-> T] {`)
        // Field: starts with a type and an identifier name.
        if (check(TokenType::IDENTIFIER) && looksLikeFuncDecl()) {
            auto method = parseFuncDecl(memberVis);
            decl->methods.push_back(std::move(method));
            continue;
        }

        StructField field;
        field.visibility = memberVis;
        field.type = parseType();
        field.line = field.type ? field.type->line : kw.getLine();
        field.column = field.type ? field.type->column : kw.getColumn();
        const Token& fnameTok = expect(TokenType::IDENTIFIER, "field name");
        field.name = fnameTok.getText();
        expect(TokenType::SEMI, "';' after field declaration");
        decl->fields.push_back(std::move(field));
    }
    expect(TokenType::R_BRACE, "'}' to close struct");
    return decl;
}

bool Parser::looksLikeTypedDecl() const {
    size_t i = pos + 1;
    if (i < tokens.size() && tokens[i].getType() == TokenType::QUES) {
        i++;
    }
    if (i >= tokens.size() || tokens[i].getType() != TokenType::IDENTIFIER) return false;
    i++;
    if (i >= tokens.size()) return false;
    TokenType after = tokens[i].getType();
    return after == TokenType::EQ || after == TokenType::SEMI;
}

std::unique_ptr<BlockStmt> Parser::parseBlock() {
    const Token& open = expect(TokenType::L_BRACE, "'{'");
    auto block = std::make_unique<BlockStmt>();
    block->line = open.getLine();
    block->column = open.getColumn();
    while (!check(TokenType::R_BRACE) && !atEnd()) {
        block->statements.push_back(parseStatement());
    }
    expect(TokenType::R_BRACE, "'}'");
    return block;
}

StmtPtr Parser::parseLet() {
    const Token& kw = expect(TokenType::LET, "'let'");
    const Token& name = expect(TokenType::IDENTIFIER, "identifier after 'let'");
    TypePtr type;
    if (match(TokenType::COLON)) {
        type = parseType();
    }
    ExprPtr init;
    if (match(TokenType::EQ)) {
        init = parseExpression();
    }
    expect(TokenType::SEMI, "';' after let declaration");
    auto s = std::make_unique<VarDeclStmt>(std::move(type), name.getText(), std::move(init));
    s->line = kw.getLine();
    s->column = kw.getColumn();
    return s;
}

StmtPtr Parser::parseTypedVarDecl() {
    auto type = parseType();
    int line = type->line;
    int column = type->column;
    const Token& name = expect(TokenType::IDENTIFIER, "identifier after type");
    ExprPtr init;
    if (match(TokenType::EQ)) {
        init = parseExpression();
    }
    expect(TokenType::SEMI, "';' after declaration");
    auto s = std::make_unique<VarDeclStmt>(std::move(type), name.getText(), std::move(init));
    s->line = line;
    s->column = column;
    return s;
}

StmtPtr Parser::parseReturn() {
    const Token& kw = expect(TokenType::RETURN, "'return'");
    ExprPtr e;
    if (!check(TokenType::SEMI)) {
        e = parseExpression();
    }
    expect(TokenType::SEMI, "';' after return");
    auto s = std::make_unique<ReturnStmt>(std::move(e));
    s->line = kw.getLine();
    s->column = kw.getColumn();
    return s;
}

StmtPtr Parser::parseIf() {
    const Token& kw = expect(TokenType::IF, "'if'");
    auto cond = parseExpression();
    auto thenB = parseBlock();
    StmtPtr elseB;
    if (match(TokenType::ELSE)) {
        if (check(TokenType::IF)) {
            elseB = parseIf();
        } else {
            elseB = parseBlock();
        }
    }
    auto s = std::make_unique<IfStmt>(std::move(cond), std::move(thenB), std::move(elseB));
    s->line = kw.getLine();
    s->column = kw.getColumn();
    return s;
}

StmtPtr Parser::parseWhile() {
    const Token& kw = expect(TokenType::WHILE, "'while'");
    auto cond = parseExpression();
    auto body = parseBlock();
    auto s = std::make_unique<WhileStmt>(std::move(cond), std::move(body));
    s->line = kw.getLine();
    s->column = kw.getColumn();
    return s;
}

StmtPtr Parser::parseExprStmt() {
    auto e = parseExpression();
    int line = e->line;
    int column = e->column;
    expect(TokenType::SEMI, "';' after expression");
    auto s = std::make_unique<ExprStmt>(std::move(e));
    s->line = line;
    s->column = column;
    return s;
}

ExprPtr Parser::parsePrefix() {
    const Token& tok = peek();
    int line = tok.getLine();
    int column = tok.getColumn();

    switch (tok.getType()) {
        case TokenType::INT_LITERAL:
        case TokenType::LONG_LITERAL: {
            consume();
            auto e = std::make_unique<IntLitExpr>(parseIntText(tok.getText()));
            e->line = line;
            e->column = column;
            return e;
        }
        case TokenType::DOUBLE_LITERAL:
        case TokenType::FLOAT_LITERAL: {
            consume();
            auto e = std::make_unique<DoubleLitExpr>(parseDoubleText(tok.getText()));
            e->line = line;
            e->column = column;
            return e;
        }
        case TokenType::STRING_LITERAL: {
            consume();
            auto e = std::make_unique<StringLitExpr>(tok.getText());
            e->line = line;
            e->column = column;
            return e;
        }
        case TokenType::IDENTIFIER: {
            consume();
            auto e = std::make_unique<IdentExpr>(tok.getText());
            e->line = line;
            e->column = column;
            return e;
        }
        case TokenType::THIS: {
            consume();
            auto e = std::make_unique<ThisExpr>();
            e->line = line;
            e->column = column;
            return e;
        }
        case TokenType::TRUE_KW:
        case TokenType::FALSE_KW: {
            bool v = tok.getType() == TokenType::TRUE_KW;
            consume();
            auto e = std::make_unique<BoolLitExpr>(v);
            e->line = line;
            e->column = column;
            return e;
        }
        case TokenType::NULL_KW: {
            consume();
            auto e = std::make_unique<NullLitExpr>();
            e->line = line;
            e->column = column;
            return e;
        }
        case TokenType::L_PAREN: {
            consume();
            auto inner = parseExpression();
            expect(TokenType::R_PAREN, "')'");
            return inner;
        }
        case TokenType::SUB:
        case TokenType::NOT:
        case TokenType::PLUS_PLUS:
        case TokenType::SUB_SUB: {
            Token op = consume();
            auto operand = parsePrecedence(PREC_UNARY);
            auto e = std::make_unique<UnaryExpr>(op.getType(), std::move(operand));
            e->line = line;
            e->column = column;
            return e;
        }
        default:
            error(tok, "Unexpected token in expression");
    }
}

ExprPtr Parser::parsePrecedence(int minPrec) {
    auto left = parsePrefix();

    while (true) {
        const Token& opTok = peek();
        TokenType tt = opTok.getType();
        int prec = infixPrecedence(tt);
        if (prec < minPrec) break;

        if (tt == TokenType::L_PAREN) {
            consume();
            std::vector<ExprPtr> args;
            if (!check(TokenType::R_PAREN)) {
                args.push_back(parseExpression());
                while (match(TokenType::COMMA)) {
                    args.push_back(parseExpression());
                }
            }
            expect(TokenType::R_PAREN, "')' after arguments");
            int line = left->line;
            int column = left->column;
            left = std::make_unique<CallExpr>(std::move(left), std::move(args));
            left->line = line;
            left->column = column;
        } else if (tt == TokenType::L_BRACKET) {
            consume();
            auto index = parseExpression();
            expect(TokenType::R_BRACKET, "']' after subscript");
            int line = left->line;
            int column = left->column;
            left = std::make_unique<SubscriptExpr>(std::move(left), std::move(index));
            left->line = line;
            left->column = column;
        } else if (tt == TokenType::DOT) {
            consume();
            const Token& nameTok = expect(TokenType::IDENTIFIER, "identifier after '.'");
            int line = left->line;
            int column = left->column;
            left = std::make_unique<MemberExpr>(std::move(left), nameTok.getText());
            left->line = line;
            left->column = column;
        } else if (tt == TokenType::QUES) {
            consume();
            auto thenE = parseExpression();
            expect(TokenType::COLON, "':' in ternary expression");
            auto elseE = parsePrecedence(1);  // right-assoc; allow assignment on RHS
            int line = left->line;
            int column = left->column;
            left = std::make_unique<TernaryExpr>(std::move(left), std::move(thenE), std::move(elseE));
            left->line = line;
            left->column = column;
        } else if (isAssignmentOp(tt)) {
            consume();
            auto right = parsePrecedence(prec);
            int line = left->line;
            int column = left->column;
            left = std::make_unique<AssignExpr>(tt, std::move(left), std::move(right));
            left->line = line;
            left->column = column;
        } else {
            consume();
            int nextMin = isRightAssoc(tt) ? prec : prec + 1;
            auto right = parsePrecedence(nextMin);
            int line = left->line;
            int column = left->column;
            left = std::make_unique<BinaryExpr>(tt, std::move(left), std::move(right));
            left->line = line;
            left->column = column;
        }
    }
    return left;
}
