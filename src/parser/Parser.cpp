#include "Parser.h"
#include "../diagnostics/Diagnostic.h"
#include <string>

static constexpr int PREC_UNARY = 12;
static constexpr int PREC_POSTFIX = 13;

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

int Parser::infixPrecedence(TokenType type) const {
    if (isAssignmentOp(type)) return 1;
    switch (type) {
        case TokenType::OR:        return 2;
        case TokenType::AND:       return 3;
        case TokenType::BIT_OR:    return 4;
        case TokenType::CARET:     return 5;
        case TokenType::BIT_AND:   return 6;
        case TokenType::EQ_EQ:
        case TokenType::NOT_EQ:    return 7;
        case TokenType::LT:
        case TokenType::GT:
        case TokenType::LT_EQ:
        case TokenType::GT_EQ:     return 8;
        case TokenType::LT_LT:
        case TokenType::GT_GT:
        case TokenType::GT_GT_GT:  return 9;
        case TokenType::PLUS:
        case TokenType::SUB:       return 10;
        case TokenType::STAR:
        case TokenType::SLASH:
        case TokenType::PERCENT:   return 11;
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

    switch (tok.getType()) {
        case TokenType::INT_LITERAL:
        case TokenType::LONG_LITERAL: {
            consume();
            auto e = std::make_unique<IntLitExpr>(parseIntText(tok.getText()));
            e->line = line;
            return e;
        }
        case TokenType::DOUBLE_LITERAL:
        case TokenType::FLOAT_LITERAL: {
            consume();
            auto e = std::make_unique<DoubleLitExpr>(parseDoubleText(tok.getText()));
            e->line = line;
            return e;
        }
        case TokenType::STRING_LITERAL: {
            consume();
            auto e = std::make_unique<StringLitExpr>(tok.getText());
            e->line = line;
            return e;
        }
        case TokenType::IDENTIFIER: {
            consume();
            auto e = std::make_unique<IdentExpr>(tok.getText());
            e->line = line;
            return e;
        }
        case TokenType::TRUE_KW:
        case TokenType::FALSE_KW: {
            bool v = tok.getType() == TokenType::TRUE_KW;
            consume();
            auto e = std::make_unique<BoolLitExpr>(v);
            e->line = line;
            return e;
        }
        case TokenType::NULL_KW: {
            consume();
            auto e = std::make_unique<NullLitExpr>();
            e->line = line;
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
            left = std::make_unique<CallExpr>(std::move(left), std::move(args));
            left->line = line;
        } else if (tt == TokenType::L_BRACKET) {
            consume();
            auto index = parseExpression();
            expect(TokenType::R_BRACKET, "']' after subscript");
            int line = left->line;
            left = std::make_unique<SubscriptExpr>(std::move(left), std::move(index));
            left->line = line;
        } else if (tt == TokenType::DOT) {
            consume();
            const Token& nameTok = expect(TokenType::IDENTIFIER, "identifier after '.'");
            int line = left->line;
            left = std::make_unique<MemberExpr>(std::move(left), nameTok.getText());
            left->line = line;
        } else if (isAssignmentOp(tt)) {
            consume();
            auto right = parsePrecedence(prec);
            int line = left->line;
            left = std::make_unique<AssignExpr>(tt, std::move(left), std::move(right));
            left->line = line;
        } else {
            consume();
            int nextMin = isRightAssoc(tt) ? prec : prec + 1;
            auto right = parsePrecedence(nextMin);
            int line = left->line;
            left = std::make_unique<BinaryExpr>(tt, std::move(left), std::move(right));
            left->line = line;
        }
    }
    return left;
}
