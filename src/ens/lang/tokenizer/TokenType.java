package ens.lang.tokenizer;

public enum TokenType {
    EOF(),
    ERROR,
    L_PAREN("("),
    R_PAREN(")"),
    L_BRACE("{"),
    R_BRACE("}"),
    L_BRACKET("["),
    R_BRACKET("]"),
    SEMI(";"),
    COMMA(","),
    DOT("."),
    ELLIPSIS("..."),
    PLUS("+"),
    SUB("-"),
    STAR("*"),
    SLASH("/"),
    EQ("="),
    GT(">"),
    LT("<"),
    NOT("!"),
    QUES("?"),
    COLON(":"),
    EQ_EQ("=="),
    LT_EQ("<="),
    GT_EQ(">="),
    NOT_EQ("!="),
    ARROW("=>"),
    AND("&&"),
    OR("||"),
    BIT_AND("&"),
    BIT_OR("|"),
    PLUS_PLUS("++"),
    SUB_SUB("--"),
    CARET("^"),
    PERCENT("%"),
    LT_LT("<<"),
    GT_GT(">>"),
    GT_GT_GT(">>>"),
    PLUS_EQ("+="),
    SUB_EQ("-="),
    STAR_EQ("*="),
    SLASH_EQ("/="),
    BIT_AND_EQ("&="),
    BIT_OR_EQ("|="),
    CARET_EQ("^="),
    PERCENT_EQ("%="),
    LT_LT_EQ("<<="),
    GT_GT_EQ(">>="),
    GT_GT_GT_EQ(">>>="),
    BYTE("byte"),
    CHAR("char"),
    INT("int"),
    LONG("long"),
    FLOAT("float"),
    DOUBLE("double"),
    SHORT("short"),
    BOOLEAN("boolean"),
    CHAR_LITERAL,
    INT_LITERAL,
    LONG_LITERAL,
    FLOAT_LITERAL,
    DOUBLE_LITERAL,
    STRING_LITERAL,
    IDENTIFIER(),
    ASSERT("assert"),
    BREAK("break"),
    CASE("case"),
    CATCH("catch"),
    CLASS("class"),
    CONST("const"),
    CONTINUE("continue"),
    DEFAULT("default"),
    DO("do"),
    ELSE("else"),
    ENUM("enum"),
    EXTENDS("extends"),
    FINAL("final"),
    FINALLY("finally"),
    FOR("for"),
    GOTO("goto"),
    IF("if"),
    IMPORT("import"),
    NEW("new"),
    PACKAGE("package"),
    PUBLIC("public"),
    RETURN("return"),
    STATIC("static"),
    SUPER("super"),
    SWITCH("switch"),
    THIS("this"),
    THROW("throw"),
    THROWS("throws"),
    TRY("try"),
    VOID("void"),
    WHILE("while");

    private String name;

    TokenType() {
    }

    TokenType(String name) {
        this.name = name;
    }

    public String getName() {
        return name;
    }

    public static TokenType lookup(String name) {
        for (TokenType t : TokenType.values()) {
            if (name.equals(t.name)) {
                return t;
            }
        }
        return IDENTIFIER;
    }
}
