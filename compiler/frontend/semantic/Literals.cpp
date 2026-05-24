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