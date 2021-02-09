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
        StringBuilder identifier = new StringBuilder();
        identifier.append(scanner.currentChar());
        identifier.append(scanner.scanChar());
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
                default:
                    if (reader.ch < '\u0080') {
                        // all ASCII range chars already handled, above
                        isJavaIdentifierPart = false;
                    } else {
                        if (Character.isIdentifierIgnorable(reader.ch)) {
                            scanner.scanChar();
                            continue;
                        } else {
                            int codePoint = reader.peekSurrogates();
                            if (codePoint >= 0) {
                                if (isJavaIdentifierPart = Character.isJavaIdentifierPart(codePoint)) {
                                    reader.putChar(true);
                                }
                            } else {
                                isJavaIdentifierPart = Character.isJavaIdentifierPart(reader.ch);
                            }
                        }
                    }
                    if (!isJavaIdentifierPart) {
                        name = reader.name();
                        tk = tokens.lookupKind(name);
                        return;
                    }
            }
            identifier.append(scanner.scanChar());
        } while (true);
    }
}
