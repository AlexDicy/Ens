#include "Literals.h"

#include <cstdint>
#include <string>

bool parseIntegerLiteralMagnitude(std::u16string_view text, uint64_t& out) {
    out = 0;
    std::string s;
    s.reserve(text.size());
    for (char16_t c : text) {
        if (c == u'_') continue;
        if (c == u'l' || c == u'L') continue;  // type suffix
        s.push_back(static_cast<char>(c));
    }

    int base = 10;
    size_t i = 0;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; i = 2; }
    else if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) { base = 2; i = 2; }

    if (i == s.size()) return false;  // empty after stripping prefix/suffix

    uint64_t v = 0;
    for (; i < s.size(); ++i) {
        char c = s[i];
        unsigned digit;
        if (c >= '0' && c <= '9')      digit = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
        else return false;
        if (digit >= static_cast<unsigned>(base)) return false;
        if (v > (UINT64_MAX - digit) / static_cast<uint64_t>(base)) return false;
        v = v * static_cast<uint64_t>(base) + digit;
    }
    out = v;
    return true;
}

static int hexDigitValue(char16_t c) {
    if (c >= u'0' && c <= u'9') return static_cast<int>(c - u'0');
    if (c >= u'a' && c <= u'f') return static_cast<int>(c - u'a') + 10;
    if (c >= u'A' && c <= u'F') return static_cast<int>(c - u'A') + 10;
    return -1;
}

uint32_t decodeEscapeSequence(std::u16string_view text, size_t backslashIndex,
                              size_t end, size_t& next) {
    char16_t n = text[backslashIndex + 1];
    next = backslashIndex + 2;
    switch (n) {
        case u'n':  return 0x0A;
        case u't':  return 0x09;
        case u'r':  return 0x0D;
        case u'b':  return 0x08;
        case u'f':  return 0x0C;
        case u'0':  return 0x00;
        case u'\\': return 0x5C;
        case u'\'': return 0x27;
        case u'"':  return 0x22;
        case u'u': {
            // \uXXXX names a BMP scalar with exactly four hex digits. A malformed
            // sequence falls back to the letter 'u', matching the reference
            // tokenizer, which does not special-case it.
            if (backslashIndex + 6 <= end) {
                int h0 = hexDigitValue(text[backslashIndex + 2]);
                int h1 = hexDigitValue(text[backslashIndex + 3]);
                int h2 = hexDigitValue(text[backslashIndex + 4]);
                int h3 = hexDigitValue(text[backslashIndex + 5]);
                if (h0 >= 0 && h1 >= 0 && h2 >= 0 && h3 >= 0) {
                    next = backslashIndex + 6;
                    return static_cast<uint32_t>((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                }
            }
            return static_cast<uint32_t>(n);
        }
        default: return static_cast<uint32_t>(n);
    }
}

uint32_t parseCharLiteralCodepoint(std::u16string_view text) {
    size_t start = 0, end = text.size();
    if (end >= 2 && text.front() == u'\'' && text.back() == u'\'') { start = 1; end--; }
    if (start >= end) return 0;
    char16_t c = text[start];
    if (c == u'\\' && start + 1 < end) {
        size_t next;
        return decodeEscapeSequence(text, start, end, next);
    }
    // UTF-16 surrogate pair: codepoints above U+FFFF arrive as two char16_t
    // units (high in D800..DBFF, low in DC00..DFFF) which we recombine here.
    if (c >= 0xD800 && c <= 0xDBFF && start + 1 < end) {
        char16_t low = text[start + 1];
        if (low >= 0xDC00 && low <= 0xDFFF) {
            return 0x10000u + ((static_cast<uint32_t>(c) - 0xD800u) << 10) +
                   (static_cast<uint32_t>(low) - 0xDC00u);
        }
    }
    return static_cast<uint32_t>(c);
}