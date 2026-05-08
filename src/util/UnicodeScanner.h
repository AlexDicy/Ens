#pragma once
#include <string>
#include <vector>

class UnicodeScanner {
public:
    static constexpr char16_t TAB = 0x9;
    static constexpr char16_t LF  = 0xA;
    static constexpr char16_t FF  = 0xC;
    static constexpr char16_t CR  = 0xD;
    static constexpr char16_t EOI = 0x1A;

    std::vector<char16_t> buffer;
    int pointer = 0;
    char16_t c = 0;

    int unicodeConversionPointer = -1;

    std::vector<char16_t> sbuffer;
    int realLength = 0;
    int spointer = 0;

    int line = 1;
    int column = 0;
    bool prevWasCR = false;

    explicit UnicodeScanner(std::u16string_view src);

    char16_t scanChar();
    void scanCommentChar();
    bool canScan();
    void putChar(char16_t ch, bool scan);
    void putChar(char16_t ch);
    void putChar(bool scan);
    void nextChar(bool skip);
    std::u16string getSaved() const;
    void repeat(char16_t ch, int count);
    void reset(int pos);
    int digit(int pos, int base);
    bool isUnicode() const;
    void skipChar();
    char16_t peekChar() const;
    int peekSurrogates();

private:
    void convertUnicode();
};
