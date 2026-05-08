#include "UnicodeScanner.h"

#include <iostream>

static int charDigit(int ch, int base) {
    int d;
    if ('0' <= ch && ch <= '9')      d = ch - '0';
    else if ('a' <= ch && ch <= 'z') d = ch - 'a' + 10;
    else if ('A' <= ch && ch <= 'Z') d = ch - 'A' + 10;
    else return -1;
    return d < base ? d : -1;
}

UnicodeScanner::UnicodeScanner(std::u16string_view src) {
    buffer.assign(src.begin(), src.end());
    buffer.push_back(EOI);
    realLength = (int)src.size();
    pointer = -1;
    unicodeConversionPointer = -1;
    sbuffer.resize(128);
    spointer = 0;
    c = 0;
    scanChar();
}

char16_t UnicodeScanner::scanChar() {
    if (pointer < (int)buffer.size()) {
        if (c == LF) {
            if (!prevWasCR) {
                line++;
            }
            column = 1;
            prevWasCR = false;
        } else if (c == CR) {
            line++;
            column = 1;
            prevWasCR = true;
        } else {
            column++;
            prevWasCR = false;
        }
        c = buffer[++pointer];
        if (c == u'\\') {
            convertUnicode();
        }
    }
    return c;
}

void UnicodeScanner::scanCommentChar() {
    scanChar();
    if (c == u'\\') {
        if (peekChar() == u'\\' && !isUnicode()) {
            skipChar();
        } else {
            convertUnicode();
        }
    }
}

bool UnicodeScanner::canScan() {
    return pointer < (int)buffer.size();
}

void UnicodeScanner::putChar(char16_t ch, bool scan) {
    if (spointer >= (int)sbuffer.size()) {
        sbuffer.resize(spointer + 8);
    }
    sbuffer[spointer++] = ch;
    if (scan)
        scanChar();
}

void UnicodeScanner::putChar(char16_t ch) {
    putChar(ch, false);
}

void UnicodeScanner::putChar(bool scan) {
    putChar(c, scan);
}

void UnicodeScanner::nextChar(bool skip) {
    if (!skip) {
        if (spointer >= (int)sbuffer.size()) {
            sbuffer.resize(spointer + 8);
        }
        sbuffer[spointer++] = c;
    }
    scanChar();
}

std::u16string UnicodeScanner::getSaved() const {
    return std::u16string(sbuffer.data(), spointer);
}

void UnicodeScanner::repeat(char16_t ch, int count) {
    for (; 0 < count; count--) {
        putChar(ch, false);
    }
}

void UnicodeScanner::reset(int pos) {
    pointer = pos - 1;
    scanChar();
}

int UnicodeScanner::digit(int pos, int base) {
    char16_t ch = c;
    if (u'0' <= ch && ch <= u'9')
        return charDigit(ch, base);
    int codePoint = peekSurrogates();
    int result = codePoint >= 0 ? charDigit(codePoint, base) : charDigit(ch, base);
    if (result >= 0 && ch > 0x7f) {
        std::cerr << "Illegal non-ascii digit, index: " << (pos + 1) << '\n';
        if (codePoint >= 0)
            scanChar();
        c = u"0123456789abcdef"[result];
    }
    return result;
}

bool UnicodeScanner::isUnicode() const {
    return unicodeConversionPointer == pointer;
}

void UnicodeScanner::skipChar() {
    pointer++;
}

char16_t UnicodeScanner::peekChar() const {
    return buffer[pointer + 1];
}

int UnicodeScanner::peekSurrogates() {
    if (c >= 0xD800 && c <= 0xDBFF) {
        char16_t high = c;
        int prevBP = pointer;
        scanChar();
        char16_t low = c;
        c = high;
        pointer = prevBP;
        if (low >= 0xDC00 && low <= 0xDFFF) {
            return (int)(((char32_t)(high - 0xD800) << 10) + (low - 0xDC00) + 0x10000);
        }
    }
    return -1;
}

void UnicodeScanner::convertUnicode() {
    if (c == u'\\' && unicodeConversionPointer != pointer) {
        pointer++;
        c = buffer[pointer];
        if (c == u'u') {
            do {
                pointer++;
                c = buffer[pointer];
            } while (c == u'u');
            int limit = pointer + 3;
            if (limit < (int)buffer.size()) {
                int d = charDigit(c, 16);
                int code = d;
                while (pointer < limit && d >= 0) {
                    pointer++;
                    c = buffer[pointer];
                    d = charDigit(c, 16);
                    code = (code << 4) + d;
                }
                if (d >= 0) {
                    c = (char16_t)code;
                    unicodeConversionPointer = pointer;
                    return;
                }
            }
            std::cerr << "Invalid Unicode escape, index: " << pointer << '\n';
        } else {
            pointer--;
            c = u'\\';
        }
    }
}
