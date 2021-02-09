package ens.lang.tokenizer;

public enum TokenType {
    L_PAREN("("),
    R_PAREN(")"),
    L_BRACE("{"),
    R_BRACE("}"),
    L_BRACKET("["),
    R_BRACKET("]"),
    SEMI(";"),
    COMMA(","),
    DOT("."),
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
    CHARLITERAL,
    INTLITERAL,
    LONGLITERAL,
    FLOATLITERAL,
    DOUBLELITERAL,
    STRINGLITERA;

    private String name;

    TokenType() {
    }

    TokenType(String name) {
        this.name = name;
    }

    public String getName() {
        return name;
    }
}
