#include "TokenType.h"

#include <array>
#include <utility>

static constexpr std::pair<TokenType, std::u16string_view> TOKEN_NAMES[] = {
    {TokenType::L_PAREN,      u"("},
    {TokenType::R_PAREN,      u")"},
    {TokenType::L_BRACE,      u"{"},
    {TokenType::R_BRACE,      u"}"},
    {TokenType::L_BRACKET,    u"["},
    {TokenType::R_BRACKET,    u"]"},
    {TokenType::SEMI,         u";"},
    {TokenType::COMMA,        u","},
    {TokenType::DOT,          u"."},
    {TokenType::ELLIPSIS,     u"..."},
    {TokenType::PLUS,         u"+"},
    {TokenType::SUB,          u"-"},
    {TokenType::STAR,         u"*"},
    {TokenType::SLASH,        u"/"},
    {TokenType::EQ,           u"="},
    {TokenType::GT,           u">"},
    {TokenType::LT,           u"<"},
    {TokenType::NOT,          u"!"},
    {TokenType::QUES,         u"?"},
    {TokenType::COLON,        u":"},
    {TokenType::EQ_EQ,        u"=="},
    {TokenType::LT_EQ,        u"<="},
    {TokenType::GT_EQ,        u">="},
    {TokenType::NOT_EQ,       u"!="},
    {TokenType::ARROW,        u"=>"},
    {TokenType::AND,          u"&&"},
    {TokenType::OR,           u"||"},
    {TokenType::BIT_AND,      u"&"},
    {TokenType::BIT_OR,       u"|"},
    {TokenType::PLUS_PLUS,    u"++"},
    {TokenType::SUB_SUB,      u"--"},
    {TokenType::CARET,        u"^"},
    {TokenType::PERCENT,      u"%"},
    {TokenType::LT_LT,        u"<<"},
    {TokenType::GT_GT,        u">>"},
    {TokenType::GT_GT_GT,     u">>>"},
    {TokenType::PLUS_EQ,      u"+="},
    {TokenType::SUB_EQ,       u"-="},
    {TokenType::STAR_EQ,      u"*="},
    {TokenType::SLASH_EQ,     u"/="},
    {TokenType::BIT_AND_EQ,   u"&="},
    {TokenType::BIT_OR_EQ,    u"|="},
    {TokenType::CARET_EQ,     u"^="},
    {TokenType::PERCENT_EQ,   u"%="},
    {TokenType::LT_LT_EQ,    u"<<="},
    {TokenType::GT_GT_EQ,    u">>="},
    {TokenType::GT_GT_GT_EQ, u">>>="},
    {TokenType::BYTE,         u"byte"},
    {TokenType::CHAR,         u"char"},
    {TokenType::INT,          u"int"},
    {TokenType::LONG,         u"long"},
    {TokenType::FLOAT,        u"float"},
    {TokenType::DOUBLE,       u"double"},
    {TokenType::SHORT,        u"short"},
    {TokenType::BOOLEAN,      u"boolean"},
    {TokenType::ASSERT,       u"assert"},
    {TokenType::BREAK,        u"break"},
    {TokenType::CASE,         u"case"},
    {TokenType::CATCH,        u"catch"},
    {TokenType::CLASS,        u"class"},
    {TokenType::CONST,        u"const"},
    {TokenType::CONTINUE,     u"continue"},
    {TokenType::DEFAULT,      u"default"},
    {TokenType::DO,           u"do"},
    {TokenType::ELSE,         u"else"},
    {TokenType::ENUM,         u"enum"},
    {TokenType::EXTENDS,      u"extends"},
    {TokenType::FINAL,        u"final"},
    {TokenType::FINALLY,      u"finally"},
    {TokenType::FOR,          u"for"},
    {TokenType::GOTO,         u"goto"},
    {TokenType::IF,           u"if"},
    {TokenType::IMPORT,       u"import"},
    {TokenType::NEW,          u"new"},
    {TokenType::PACKAGE,      u"package"},
    {TokenType::PUBLIC,       u"public"},
    {TokenType::RETURN,       u"return"},
    {TokenType::STATIC,       u"static"},
    {TokenType::SUPER,        u"super"},
    {TokenType::SWITCH,       u"switch"},
    {TokenType::THIS,         u"this"},
    {TokenType::THROW,        u"throw"},
    {TokenType::THROWS,       u"throws"},
    {TokenType::TRY,          u"try"},
    {TokenType::VOID,         u"void"},
    {TokenType::WHILE,        u"while"},
};

std::u16string_view getTokenName(TokenType type) {
    for (const auto& [t, name] : TOKEN_NAMES) {
        if (t == type) return name;
    }
    return {};
}

TokenType lookupToken(std::u16string_view name) {
    for (const auto& [type, n] : TOKEN_NAMES) {
        if (n == name) return type;
    }
    return TokenType::IDENTIFIER;
}
