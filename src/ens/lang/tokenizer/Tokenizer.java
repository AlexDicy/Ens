package ens.lang.tokenizer;

import ens.lang.util.UnicodeScanner;

import java.util.ArrayList;
import java.util.List;

import static ens.lang.util.UnicodeScanner.*;

public class Tokenizer {
    private UnicodeScanner scanner;
    private TokenType tokenType;

    /**
     * The token's radix, set by readToken()
     */
    protected int radix;

    /**
     * If is a text block, set by readToken()
     */
    protected boolean isTextBlock;

    /**
     * If contains escape sequences, set by readToken()
     */
    protected boolean hasEscapeSequences;


    public static List<Token> tokenize(String code) {
        List<Token> tokens = new ArrayList<>();
        if (code.length() > 0) {
            Tokenizer tokenizer = new Tokenizer();
            tokenizer.scanner = new UnicodeScanner(code.toCharArray());

            Token token = tokenizer.readToken();
            while (token != null && token.getType() != TokenType.EOF) {
                tokens.add(token);
                token = tokenizer.readToken();
            }
            for (Token t : tokens) {
                System.out.println(">" + t.getType() + " - " + t.getText());
            }
        }
        return tokens;
    }

    public Token readToken() {
        // reset saved buffer
        scanner.spointer = 0;
        // save start position
        var pos = scanner.pointer;
        var loop = true;
        while (loop) {
            switch (scanner.c) {
                case ' ':
                case '\t':
                case FF:
                    scanner.scanChar();
                    while (scanner.c == ' ' || scanner.c == '\t') {
                        scanner.scanChar();
                    }
                    break;
                case LF:
                    scanner.scanChar();
                    break;
                case CR:
                    scanner.scanChar();
                    if (scanner.c == LF) {
                        scanner.scanChar();
                    }
                    break;
                //@formatter:off
                case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
                case 'G': case 'H': case 'I': case 'J': case 'K': case 'L':
                case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R':
                case 'S': case 'T': case 'U': case 'V': case 'W': case 'X':
                case 'Y': case 'Z':
                case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
                case 'g': case 'h': case 'i': case 'j': case 'k': case 'l':
                case 'm': case 'n': case 'o': case 'p': case 'q': case 'r':
                case 's': case 't': case 'u': case 'v': case 'w': case 'x':
                case 'y': case 'z':
                case '$': case '_':
                //@formatter:on
                    scanIdentifier();
                    loop = false;
                    break;
                case '0':
                    scanner.scanChar();
                    if (scanner.c == 'x' || scanner.c == 'X') {
                        scanner.scanChar();
                        scanNumber(pos, 16);
                    } else if (scanner.c == 'b' || scanner.c == 'B') {
                        scanner.scanChar();
                        scanNumber(pos, 2);
                    } else {
                        scanner.putChar('0');
                        if (scanner.c == '_') {
                            int savePos = scanner.pointer;
                            do {
                                scanner.scanChar();
                            } while (scanner.c == '_');
                            if (scanner.digit(pos, 10) < 0) {
                                error(savePos, "Illegal Underscore");
                            }
                        }
                        scanNumber(pos, 8);
                    }
                    loop = false;
                    break;
                //@formatter:off
                case '1': case '2': case '3':
                case '4': case '5': case '6':
                case '7': case '8': case '9':
                //@formatter:on
                    scanNumber(pos, 10);
                    loop = false;
                    break;
                case '.':
                    scanner.scanChar();
                    if (scanner.digit(pos, 10) >= 0) {
                        scanner.putChar('.');
                        scanFractionAndSuffix(pos);
                    } else if (scanner.c == '.') {
                        int savePos = scanner.pointer;
                        scanner.putChar('.');
                        scanner.putChar('.', true);
                        if (scanner.c == '.') {
                            scanner.scanChar();
                            scanner.putChar('.');
                            tokenType = TokenType.ELLIPSIS;
                        } else {
                            error(savePos, "Illegal Dot");
                        }
                    } else {
                        tokenType = TokenType.DOT;
                    }
                    loop = false;
                    break;
                case ',':
                    scanner.scanChar();
                    tokenType = TokenType.COMMA;
                    loop = false;
                    break;
                case ';':
                    scanner.scanChar();
                    tokenType = TokenType.SEMI;
                    loop = false;
                    break;
                case '(':
                    scanner.scanChar();
                    tokenType = TokenType.L_PAREN;
                    loop = false;
                    break;
                case ')':
                    scanner.scanChar();
                    tokenType = TokenType.R_PAREN;
                    loop = false;
                    break;
                case '[':
                    scanner.scanChar();
                    tokenType = TokenType.L_BRACKET;
                    loop = false;
                    break;
                case ']':
                    scanner.scanChar();
                    tokenType = TokenType.R_BRACKET;
                    loop = false;
                    break;
                case '{':
                    scanner.scanChar();
                    tokenType = TokenType.L_BRACE;
                    loop = false;
                    break;
                case '}':
                    scanner.scanChar();
                    tokenType = TokenType.R_BRACE;
                    loop = false;
                    break;
                case '/':
                    scanner.scanChar();
                    // detects a comment
                    if (scanner.c == '/') {
                        do {
                            scanner.scanCommentChar();
                        } while (scanner.c != CR && scanner.c != LF && scanner.pointer < scanner.buffer.length);
                        break;
                    } else if (scanner.c == '*') {
                        scanner.scanChar();
                        while (scanner.pointer < scanner.buffer.length) {
                            if (scanner.c == '*') {
                                scanner.scanChar();
                                if (scanner.c == '/') break;
                            } else {
                                scanner.scanCommentChar();
                            }
                        }
                        if (scanner.c == '/') {
                            scanner.scanChar();
                        } else {
                            error(pos, "Unclosed Comment");
                            loop = false;
                        }
                        break;
                    } else if (scanner.c == '=') {
                        tokenType = TokenType.SLASH_EQ;
                        scanner.scanChar();
                    } else {
                        tokenType = TokenType.SLASH;
                    }
                    loop = false;
                    break;
                case '\'':
                    scanner.scanChar();
                    if (scanner.c == '\'') {
                        error(pos, "Empty CharLit");
                        scanner.scanChar();
                    } else {
                        if (scanner.c == LF || scanner.c == CR)
                            error(pos, "Illegal LineEnd In CharLit");
                        scanLitChar(pos, true, false);
                        if (scanner.c == '\'') {
                            scanner.scanChar();
                            tokenType = TokenType.CHAR_LITERAL;
                        } else {
                            error(pos, "Unclosed CharLit");
                        }
                    }
                    loop = false;
                    break;
                case '\"':
                    scanString(pos);
                    loop = false;
                    break;
                default:
                    if (isSpecial(scanner.c)) {
                        scanOperator();
                    } else {
                        if (scanner.pointer == scanner.buffer.length || scanner.c == EOI && scanner.pointer + 1 == scanner.buffer.length) {
                            tokenType = TokenType.EOF;
                            pos = scanner.realLength;
                        }
                    }
                    loop = false;
            }
        }
        return tokenType == null ? null : new Token(tokenType, scanner.getSaved(), pos, scanner.pointer);
    }

    private void error(int pos, String error) {
        tokenType = TokenType.ERROR;
    }

    /**
     * Return true if 'c' can be part of an operator.
     */
    private boolean isSpecial(char c) {
        switch (c) {
            //@formatter:off
            case '!': case '%': case '&': case '*':
            case '?': case '+': case '-': case ':':
            case '<': case '=': case '>': case '^':
            case '|': case '~': case '@':
            //@formatter:on
                return true;
            default:
                return false;
        }
    }

    /**
     * Read longest possible sequence of special characters and convert to token.
     */
    private void scanOperator() {
        while (true) {
            scanner.putChar(false);
            TokenType tk = TokenType.lookup(scanner.getSaved());
            if (tk == TokenType.IDENTIFIER) {
                scanner.spointer--;
                break;
            }
            tokenType = tk;
            scanner.scanChar();
            if (!isSpecial(scanner.c)) break;
        }
    }

    private void scanIdentifier() {
        scanner.putChar(true);
        while (true) {
            switch (scanner.c) {
                //@formatter:off
                case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
                case 'G': case 'H': case 'I': case 'J': case 'K': case 'L':
                case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R':
                case 'S': case 'T': case 'U': case 'V': case 'W': case 'X':
                case 'Y': case 'Z':
                case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
                case 'g': case 'h': case 'i': case 'j': case 'k': case 'l':
                case 'm': case 'n': case 'o': case 'p': case 'q': case 'r':
                case 's': case 't': case 'u': case 'v': case 'w': case 'x':
                case 'y': case 'z':
                case '$': case '_':
                case '0': case '1': case '2': case '3': case '4':
                case '5': case '6': case '7': case '8': case '9':
                //@formatter:on
                    scanner.putChar(true);
                    break;
                default:
                    tokenType = TokenType.lookup(scanner.getSaved());
                    return;
            }
        }
    }

    /**
     * @param radix The radix of the number; one of 2, 8, 10, 16.
     */
    private void scanNumber(int pos, int radix) {
        // for octal, allow base-10 digit in case it's a float literal
        this.radix = radix;
        int digitRadix = (radix == 8 ? 10 : radix);
        int firstDigit = scanner.digit(pos, Math.max(10, digitRadix));
        boolean seendigit = firstDigit >= 0;
        boolean seenValidDigit = firstDigit >= 0 && firstDigit < digitRadix;
        if (seendigit) {
            scanDigits(pos, digitRadix);
        }
        if (radix == 16 && scanner.c == '.') {
            scanHexFractionAndSuffix(pos, seendigit);
        } else if (seendigit && radix == 16 && (scanner.c == 'p' || scanner.c == 'P')) {
            scanHexExponentAndSuffix(pos);
        } else if (digitRadix == 10 && scanner.c == '.') {
            scanner.putChar(true);
            scanFractionAndSuffix(pos);
        } else if (digitRadix == 10 &&
                (scanner.c == 'e' || scanner.c == 'E' ||
                        scanner.c == 'f' || scanner.c == 'F' ||
                        scanner.c == 'd' || scanner.c == 'D')) {
            scanFractionAndSuffix(pos);
        } else {
            if (!seenValidDigit) {
                switch (radix) {
                    case 2:
                        error(pos, "Invalid Binary Number");
                        break;
                    case 16:
                        error(pos, "Invalid Hex Number");
                        break;
                }
            }
            if (scanner.c == 'l' || scanner.c == 'L') {
                scanner.scanChar();
                tokenType = TokenType.LONG_LITERAL;
            } else {
                tokenType = TokenType.INT_LITERAL;
            }
        }
    }

    private void scanDigits(int pos, int digitRadix) {
        char saveCh;
        int savePos;
        do {
            if (scanner.c != '_') {
                scanner.putChar(false);
            }
            saveCh = scanner.c;
            savePos = scanner.pointer;
            scanner.scanChar();
        } while (scanner.digit(pos, digitRadix) >= 0 || scanner.c == '_');
        if (saveCh == '_')
            error(savePos, "Illegal Underscore");
    }

    /**
     * Read fractional part of hexadecimal floating point number.
     */
    private void scanHexExponentAndSuffix(int pos) {
        if (scanner.c == 'p' || scanner.c == 'P') {
            scanner.putChar(true);
            if (scanner.c == '+' || scanner.c == '-') {
                scanner.putChar(true);
            }
            if (scanner.digit(pos, 10) >= 0) {
                scanDigits(pos, 10);
            } else
                error(pos, "MalformedFpLit");
        } else {
            error(pos, "MalformedFpLit");
        }
        if (scanner.c == 'f' || scanner.c == 'F') {
            scanner.putChar(true);
            tokenType = TokenType.FLOAT_LITERAL;
            radix = 16;
        } else {
            if (scanner.c == 'd' || scanner.c == 'D') {
                scanner.putChar(true);
            }
            tokenType = TokenType.DOUBLE_LITERAL;
            radix = 16;
        }
    }


    /**
     * Read fractional part of floating point number.
     */
    private void scanFraction(int pos) {
        if (scanner.digit(pos, 10) >= 0) {
            scanDigits(pos, 10);
        }
        int sp1 = scanner.spointer;
        if (scanner.c == 'e' || scanner.c == 'E') {
            scanner.putChar(true);
            if (scanner.c == '+' || scanner.c == '-') {
                scanner.putChar(true);
            }
            if (scanner.digit(pos, 10) >= 0) {
                scanDigits(pos, 10);
                return;
            }
            error(pos, "MalformedFpLit");
            scanner.spointer = sp1;
        }
    }

    /**
     * Read fractional part and 'd' or 'f' suffix of floating point number.
     */
    private void scanFractionAndSuffix(int pos) {
        radix = 10;
        scanFraction(pos);
        if (scanner.c == 'f' || scanner.c == 'F') {
            scanner.putChar(true);
            tokenType = TokenType.FLOAT_LITERAL;
        } else {
            if (scanner.c == 'd' || scanner.c == 'D') {
                scanner.putChar(true);
            }
            tokenType = TokenType.DOUBLE_LITERAL;
        }
    }

    /**
     * Read fractional part and 'd' or 'f' suffix of floating point number.
     */
    private void scanHexFractionAndSuffix(int pos, boolean seendigit) {
        radix = 16;
        assert scanner.c == '.';
        scanner.putChar(true);
        if (scanner.digit(pos, 16) >= 0) {
            seendigit = true;
            scanDigits(pos, 16);
        }
        if (!seendigit)
            error(pos, "Invalid Hex Number");
        else
            scanHexExponentAndSuffix(pos);
    }


    /**
     * Read next character in character or string literal and copy into sbuf.
     * pos - start of literal offset
     * translateEscapesNow - true if String::translateEscapes is not available
     * in the java.base libs. Occurs during bootstrapping.
     * multiline - true if scanning a text block. Allows newlines to be embedded
     * in the result.
     */
    private void scanLitChar(int pos, boolean translateEscapesNow, boolean multiline) {
        if (scanner.c == '\\') {
            if (scanner.peekChar() == '\\' && !scanner.isUnicode()) {
                scanner.skipChar();
                if (!translateEscapesNow) {
                    scanner.putChar(false);
                }
                scanner.putChar(true);
            } else {
                scanner.nextChar(translateEscapesNow);
                switch (scanner.c) {
                    //@formatter:off
                    case '0': case '1': case '2': case '3':
                    case '4': case '5': case '6': case '7':
                    //@formatter:on
                        char leadch = scanner.c;
                        int oct = scanner.digit(pos, 8);
                        scanner.nextChar(translateEscapesNow);
                        if ('0' <= scanner.c && scanner.c <= '7') {
                            oct = oct * 8 + scanner.digit(pos, 8);
                            scanner.nextChar(translateEscapesNow);
                            if (leadch <= '3' && '0' <= scanner.c && scanner.c <= '7') {
                                oct = oct * 8 + scanner.digit(pos, 8);
                                scanner.nextChar(translateEscapesNow);
                            }
                        }
                        if (translateEscapesNow) {
                            scanner.putChar((char) oct);
                        }
                        break;
                    case 'b':
                        scanner.putChar(translateEscapesNow ? '\b' : 'b', true);
                        break;
                    case 't':
                        scanner.putChar(translateEscapesNow ? '\t' : 't', true);
                        break;
                    case 'n':
                        scanner.putChar(translateEscapesNow ? '\n' : 'n', true);
                        break;
                    case 'f':
                        scanner.putChar(translateEscapesNow ? '\f' : 'f', true);
                        break;
                    case 'r':
                        scanner.putChar(translateEscapesNow ? '\r' : 'r', true);
                        break;
                    case '\'':
                    case '\"':
                    case '\\':
                        scanner.putChar(true);
                        break;
                    case 's':
                        scanner.putChar(translateEscapesNow ? ' ' : 's', true);
                        break;
                    case '\n':
                    case '\r':
                        if (!multiline) {
                            error(scanner.pointer, "Illegal Esc Char");
                        } else {
                            if (scanner.pointer == '\r' && scanner.peekChar() == '\n') {
                                scanner.nextChar(translateEscapesNow);
                            }
                            scanner.nextChar(translateEscapesNow);
                        }
                        break;
                    default:
                        error(scanner.pointer, " Illegal Esc Char");
                }
            }
        } else if (scanner.pointer != scanner.buffer.length) {
            scanner.putChar(true);
        }
    }

    /**
     * Count and skip repeated occurrences of the specified character.
     */
    private int countChar(char ch, int max) {
        int count = 0;
        for (; count < max && scanner.pointer < scanner.buffer.length && scanner.c == ch; count++) {
            scanner.scanChar();
        }
        return count;
    }


    /**
     * Skip and process a line terminator.
     */
    private void skipLineTerminator() {
        if (scanner.c == CR && scanner.peekChar() == LF) {
            scanner.scanChar();
        }
        scanner.scanChar();
    }

    /**
     * Scan a string literal or text block.
     */
    private void scanString(int pos) {
        // Clear flags.
        isTextBlock = false;
        hasEscapeSequences = false;
        // Track the end of first line for error recovery.
        int firstEOLN = -1;
        // Attempt to scan for up to 3 double quotes.
        int openCount = countChar('\"', 3);
        switch (openCount) {
            case 1: // Starting a string literal.
                break;
            case 2: // Starting an empty string literal.
                tokenType = TokenType.STRING_LITERAL;
                return;
            case 3: // Starting a text block.
                // Check if preview feature is enabled for text blocks.
                isTextBlock = true;
                // Verify the open delimiter sequence.
                while (scanner.pointer < scanner.buffer.length) {
                    char ch = scanner.c;
                    if (ch != ' ' && ch != '\t' && ch != FF) {
                        break;
                    }
                    scanner.scanChar();
                }
                if (scanner.c == LF || scanner.c == CR) {
                    skipLineTerminator();
                } else {
                    // Error if the open delimiter sequence is not
                    //     """<white space>*<LineTerminator>.
                    error(scanner.pointer, "Illegal TextBlock Open");
                    return;
                }
                break;
        }
        // While characters are available.
        while (scanner.pointer < scanner.buffer.length) {
            // If possible close delimiter sequence.
            if (scanner.c == '\"') {
                // Check to see if enough double quotes are present.
                int closeCount = countChar('\"', openCount);
                if (openCount == closeCount) {
                    // Good result.
                    tokenType = TokenType.STRING_LITERAL;
                    return;
                }
                // False alarm, add double quotes to string buffer.
                scanner.repeat('\"', closeCount);
            } else if (scanner.c == LF || scanner.c == CR) {
                // Line terminator in string literal is an error.
                // Fall out to unclosed string literal error.
                if (openCount == 1) {
                    break;
                }
                skipLineTerminator();
                // Add line terminator to string buffer.
                scanner.putChar('\n', false);
                // Record first line terminator for error recovery.
                if (firstEOLN == -1) {
                    firstEOLN = scanner.pointer;
                }
            } else if (scanner.c == '\\') {
                // Handle escape sequences.
                hasEscapeSequences = true;
                // Translate escapes immediately if TextBlockSupport is not available
                // during bootstrapping.
                scanLitChar(pos, true, openCount != 1);
            } else {
                // Add character to string buffer.
                scanner.putChar(true);
            }
        }
        // String ended without close delimiter sequence.
        error(pos, openCount == 1 ? "Unclosed StrLit" : "Unclosed TextBlock");
        if (firstEOLN != -1) {
            // Reset recovery position to point after open delimiter sequence.
            scanner.reset(firstEOLN);
        }
    }
}
