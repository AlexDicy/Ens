package ens.lang.tokenizer;

public class Token {
    private TokenType type;
    private String text;
    private int line;
    private int startColumn;
    private int endColumn;

    public Token(TokenType type, String text, int line, int startColumn) {
        this.type = type;
        this.text = text;
        this.line = line;
        this.startColumn = startColumn;
        this.endColumn = startColumn + text.length();
    }

}
