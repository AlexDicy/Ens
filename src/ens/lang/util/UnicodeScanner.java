package ens.lang.util;

public class UnicodeScanner {
    private char[] buffer;
    private int pointer;
    private int unicodeConversionPointer = -1;
    // Current char
    private char c;

    public UnicodeScanner(char[] buffer) {
        this.buffer = buffer;
        pointer = -1;
        scanChar();
    }

    public char scanChar() {
        if (pointer < buffer.length) {
            c = buffer[++pointer];
            if (c == '\\') {
                convertUnicode();
            }
        }
        return c;
    }

    public boolean canScan() {
        return pointer < buffer.length;
    }

    public char currentChar() {
        return c;
    }

    private void convertUnicode() {
        if (c == '\\' && unicodeConversionPointer != pointer) {
            pointer++;
            c = buffer[pointer];
            if (c == 'u') {
                do {
                    pointer++;
                    c = buffer[pointer];
                } while (c == 'u');
                int limit = pointer + 3;
                if (limit < buffer.length) {
                    int d = digit(pointer, 16);
                    int code = d;
                    while (pointer < limit && d >= 0) {
                        pointer++;
                        c = buffer[pointer];
                        d = digit(pointer, 16);
                        code = (code << 4) + d;
                    }
                    if (d >= 0) {
                        c = (char) code;
                        unicodeConversionPointer = pointer;
                        return;
                    }
                }
                System.err.println("Invalid Unicode escape, index: " + pointer);
            } else {
                pointer--;
                c = '\\';
            }
        }
    }

    private int peekSurrogates() {
        if (Character.isHighSurrogate(c)) {
            char high = c;
            int prevBP = pointer;

            scanChar();

            char low = c;

            c = high;
            pointer = prevBP;

            if (Character.isLowSurrogate(low)) {
                return Character.toCodePoint(high, low);
            }
        }

        return -1;
    }

    private int digit(int pos, int base) {
        char ch = c;
        if ('0' <= ch && ch <= '9')
            return Character.digit(ch, base); // a fast common case
        int codePoint = peekSurrogates();
        int result = codePoint >= 0 ? Character.digit(codePoint, base) : Character.digit(ch, base);
        if (result >= 0 && ch > 0x7f) {
            System.err.println("Illegal non-ascii digit, index: " + (pos + 1));
            if (codePoint >= 0)
                scanChar();
            c = "0123456789abcdef".charAt(result);
        }
        return result;
    }
}
