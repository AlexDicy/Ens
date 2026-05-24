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

uint32_t parseCharLiteralCodepoint(std::u16string_view text) {
    size_t start = 0, end = text.size();
    if (end >= 2 && text.front() == u'\'' && text.back() == u'\'') { start = 1; end--; }
    if (start >= end) return 0;
    char16_t c = text[start];
    if (c == u'\\' && start + 1 < end) {
        char16_t n = text[start + 1];
        switch (n) {
            case u'n':  return 0x0A;
            case u't':  return 0x09;
            case u'r':  return 0x0D;
            case u'0':  return 0x00;
            case u'\\': return 0x5C;
            case u'\'': return 0x27;
            case u'"':  return 0x22;
            default:    return static_cast<uint32_t>(n);
        }
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