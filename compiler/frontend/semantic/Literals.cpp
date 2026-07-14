#include "Literals.h"

#include <algorithm>
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

void appendUtf8(std::string& out, uint32_t scalar) {
    if (scalar < 0x80) {
        out.push_back(static_cast<char>(scalar));
    } else if (scalar < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (scalar >> 6)));
        out.push_back(static_cast<char>(0x80 | (scalar & 0x3F)));
    } else if (scalar < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (scalar >> 12)));
        out.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (scalar & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (scalar >> 18)));
        out.push_back(static_cast<char>(0x80 | ((scalar >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (scalar & 0x3F)));
    }
}

std::string utf16ToUtf8(std::u16string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char16_t c = s[i];
        uint32_t scalar = c;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size()) {
            char16_t low = s[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                scalar = 0x10000u + ((static_cast<uint32_t>(c) - 0xD800u) << 10) +
                         (static_cast<uint32_t>(low) - 0xDC00u);
                ++i;
            }
        }
        appendUtf8(out, scalar);
    }
    return out;
}

bool decodeUtf8ToUtf16(std::string_view bytes, std::u16string& out, Utf8DecodeError& err) {
    out.clear();
    out.reserve(bytes.size());
    size_t i = 0;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        i = 3;  // skip a leading UTF-8 byte order mark
    }
    auto isCont = [&](size_t j) {
        return j < bytes.size() && (static_cast<unsigned char>(bytes[j]) & 0xC0) == 0x80;
    };
    auto fail = [&](size_t at, const char* reason) {
        err.byteOffset = at;
        err.byte = static_cast<unsigned char>(bytes[at]);
        err.reason = reason;
        err.line = 1 + static_cast<size_t>(
            std::count(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(at), '\n'));
        return false;
    };
    while (i < bytes.size()) {
        unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char16_t>(c));
            i += 1;
        } else if (c < 0xC2) {
            return fail(i, c < 0xC0 ? "is a stray continuation byte with no character to continue"
                                    : "is an overlong encoding");
        } else if (c < 0xE0) {
            if (!isCont(i + 1)) return fail(i, "starts an incomplete character");
            uint32_t cp = ((c & 0x1Fu) << 6) |
                          (static_cast<unsigned char>(bytes[i + 1]) & 0x3Fu);
            out.push_back(static_cast<char16_t>(cp));
            i += 2;
        } else if (c < 0xF0) {
            if (!isCont(i + 1) || !isCont(i + 2)) return fail(i, "starts an incomplete character");
            uint32_t cp = ((c & 0x0Fu) << 12) |
                          ((static_cast<unsigned char>(bytes[i + 1]) & 0x3Fu) << 6) |
                          (static_cast<unsigned char>(bytes[i + 2]) & 0x3Fu);
            if (cp < 0x800) return fail(i, "is an overlong encoding");
            if (cp >= 0xD800 && cp <= 0xDFFF)
                return fail(i, "encodes a UTF-16 surrogate, which is not allowed");
            out.push_back(static_cast<char16_t>(cp));
            i += 3;
        } else if (c < 0xF5) {
            if (!isCont(i + 1) || !isCont(i + 2) || !isCont(i + 3))
                return fail(i, "starts an incomplete character");
            uint32_t cp = ((c & 0x07u) << 18) |
                          ((static_cast<unsigned char>(bytes[i + 1]) & 0x3Fu) << 12) |
                          ((static_cast<unsigned char>(bytes[i + 2]) & 0x3Fu) << 6) |
                          (static_cast<unsigned char>(bytes[i + 3]) & 0x3Fu);
            if (cp < 0x10000) return fail(i, "is an overlong encoding");
            if (cp > 0x10FFFF) return fail(i, "is above the highest Unicode character");
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800u | (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00u | (cp & 0x3FFu)));
            i += 4;
        } else {
            return fail(i, "is not a valid UTF-8 start byte");
        }
    }
    return true;
}

std::string describeUtf8DecodeError(const Utf8DecodeError& err) {
    static const char hex[] = "0123456789ABCDEF";
    std::string byteHex = "0x";
    byteHex.push_back(hex[(err.byte >> 4) & 0xF]);
    byteHex.push_back(hex[err.byte & 0xF]);
    return "This file is not valid UTF-8 text: byte " + byteHex + " at line " +
           std::to_string(err.line) + " " + err.reason +
           ". Save the file with UTF-8 encoding.";
}