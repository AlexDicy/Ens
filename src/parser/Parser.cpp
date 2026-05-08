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
