package ens.lang.util;

import java.util.Arrays;

public class UnicodeScanner {
    public final static byte TAB = 0x9;
    public final static byte LF = 0xA;
    public final static byte FF = 0xC;
    public final static byte CR = 0xD;
    public final static byte EOI   = 0x1A;

    public char[] buffer;
    public int pointer;
    // Current char
    public char c;

    public int unicodeConversionPointer = -1;

    // Character buffer for saved chars.
    public char[] sbuffer = new char[128];
    public int realLength;
    public int spointer;

    public UnicodeScanner(char[] buffer) {
        this.buffer = Arrays.copyOf(buffer, buffer.length + 1);
        this.buffer[buffer.length] = EOI;
        pointer = -1;
        realLength = buffer.length;
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


    /**
     * Read next character in comment, skipping over double '\' characters.
     */
    public void scanCommentChar() {
        scanChar();
        if (c == '\\') {
            if (peekChar() == '\\' && !isUnicode()) {
                skipChar();
            } else {
                convertUnicode();
            }
        }
    }

    public boolean canScan() {
        return pointer < buffer.length;
    }

    public void putChar(char c, boolean scan) {
        if (spointer >= sbuffer.length) {
            sbuffer = Arrays.copyOf(sbuffer, spointer + 8);
        }
        sbuffer[spointer++] = c;
        if (scan)
            scanChar();
    }

    public void putChar(char c) {
        putChar(c, false);
    }

    public void putChar(boolean scan) {
        putChar(c, scan);
    }

    public void nextChar(boolean skip) {
        if (!skip) {
            if (spointer >= sbuffer.length) {
                sbuffer = Arrays.copyOf(sbuffer, spointer + 8);
            }
            sbuffer[spointer++] = c;
        }

        scanChar();
    }

    public String getSaved() {
        return new String(sbuffer, 0, spointer);
    }

    /**
     * Add 'count' copies of the character 'ch' to the string buffer.
     */
    public void repeat(char ch, int count) {
        for (; 0 < count; count--) {
            putChar(ch, false);
        }
    }

    /**
     * Reset the scan buffer pointer to 'pos'.
     */
    public void reset(int pos) {
        pointer = pos - 1;
        scanChar();
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

    public int peekSurrogates() {
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

    public int digit(int pos, int base) {
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

    public boolean isUnicode() {
        return unicodeConversionPointer == pointer;
    }

    public void skipChar() {
        pointer++;
    }

    public char peekChar() {
        return buffer[pointer + 1];
    }
}
