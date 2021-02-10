package ens.lang.tokenizer;

import ens.lang.util.UnicodeScanner;

import java.util.ArrayList;
import java.util.List;

public class Tokenizer {
    private UnicodeScanner scanner;

    public static List<Token> tokenize(String code) {
        Tokenizer tokenizer = new Tokenizer();
        tokenizer.scanner = new UnicodeScanner(code.toCharArray());

        List<Token> tokens = new ArrayList<>();
        return tokens;
    }

    public Token readToken() {
        loop: while (scanner.canScan()) {
            switch (scanner.currentChar()) {
                case ' ':
                case '\t':
                case 0xA: // Line feed
                case 0xD: // Carriage return
                case 0xC: // Form feed
                    scanner.scanChar();
                    break;
                case 'A': case 'B': case 'C': case 'D': case 'E':
                case 'F': case 'G': case 'H': case 'I': case 'J':
                case 'K': case 'L': case 'M': case 'N': case 'O':
                case 'P': case 'Q': case 'R': case 'S': case 'T':
                case 'U': case 'V': case 'W': case 'X': case 'Y':
                case 'Z':
                case 'a': case 'b': case 'c': case 'd': case 'e':
                case 'f': case 'g': case 'h': case 'i': case 'j':
                case 'k': case 'l': case 'm': case 'n': case 'o':
                case 'p': case 'q': case 'r': case 's': case 't':
                case 'u': case 'v': case 'w': case 'x': case 'y':
                case 'z':
                case '$': case '_':
                    scanIdentifier();
                    break loop;
            }
        }
    }

    private void scanIdent() {
        boolean isJavaIdentifierPart;
        scanner.putChar(true);
        do {
            switch (scanner.currentChar()) {
                case 'A': case 'B': case 'C': case 'D': case 'E':
                case 'F': case 'G': case 'H': case 'I': case 'J':
                case 'K': case 'L': case 'M': case 'N': case 'O':
                case 'P': case 'Q': case 'R': case 'S': case 'T':
                case 'U': case 'V': case 'W': case 'X': case 'Y':
                case 'Z':
                case 'a': case 'b': case 'c': case 'd': case 'e':
                case 'f': case 'g': case 'h': case 'i': case 'j':
                case 'k': case 'l': case 'm': case 'n': case 'o':
                case 'p': case 'q': case 'r': case 's': case 't':
                case 'u': case 'v': case 'w': case 'x': case 'y':
                case 'z':
                case '$': case '_':
                case '0': case '1': case '2': case '3': case '4':
                case '5': case '6': case '7': case '8': case '9':
                    break;
                case '\u0000': case '\u0001': case '\u0002': case '\u0003':
                case '\u0004': case '\u0005': case '\u0006': case '\u0007':
                case '\u0008': case '\u000E': case '\u000F': case '\u0010':
                case '\u0011': case '\u0012': case '\u0013': case '\u0014':
                case '\u0015': case '\u0016': case '\u0017':
                case '\u0018': case '\u0019': case '\u001B':
                case '\u007F':
                    scanner.scanChar();
                    continue;
                case '\u001A': // EOI is also a legal identifier part
                    if (scanner.bp >= scanner.buflen) {
                        name = scanner.name();
                        tk = tokens.lookupKind(name);
                        return;
                    }
                    scanner.scanChar();
                    continue;
                default:
                    if (scanner.currentChar() < '\u0080') {
                        // all ASCII range chars already handled, above
                        isJavaIdentifierPart = false;
                    } else {
                        if (Character.isIdentifierIgnorable(scanner.currentChar())) {
                            scanner.scanChar();
                            continue;
                        } else {
                            int codePoint = scanner.peekSurrogates();
                            if (codePoint >= 0) {
                                if (isJavaIdentifierPart = Character.isJavaIdentifierPart(codePoint)) {
                                    scanner.putChar(true);
                                }
                            } else {
                                isJavaIdentifierPart = Character.isJavaIdentifierPart(scanner.currentChar());
                            }
                        }
                    }
                    if (!isJavaIdentifierPart) {
                        name = scanner.name();
                        tk = tokens.lookupKind(name);
                        return;
                    }
            }
            scanner.putChar(true);
        } while (true);
    }

    private boolean isSpecial(char ch) {
        switch (ch) {
            case '!': case '%': case '&': case '*': case '?':
            case '+': case '-': case ':': case '<': case '=':
            case '>': case '^': case '|': case '~':
            case '@':
                return true;
            default:
                return false;
        }
    }
}
