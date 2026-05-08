#include "Diagnostic.h"
#include <string>

Diagnostic::Diagnostic(DiagnosticLevel lvl, SourceSpan sp, std::string msg)
    : level(lvl), span(sp), message(std::move(msg)) {}

static const char* levelName(DiagnosticLevel level) {
    switch (level) {
        case DiagnosticLevel::Error:   return "error";
        case DiagnosticLevel::Warning: return "warning";
        case DiagnosticLevel::Note:    return "note";
    }
    return "diagnostic";
}

static void writeChar16(std::ostream& os, char16_t c) {
    if (c < 0x80) {
        os << static_cast<char>(c);
    } else if (c < 0x800) {
        os << static_cast<char>(0xC0 | (c >> 6));
        os << static_cast<char>(0x80 | (c & 0x3F));
    } else {
        os << static_cast<char>(0xE0 | (c >> 12));
        os << static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        os << static_cast<char>(0x80 | (c & 0x3F));
    }
}

void Diagnostic::print(const SourceFile& source, std::ostream& os) const {
    os << levelName(level) << ": " << message << "\n";
    os << "  --> " << source.getFilename() << ":" << span.line << ":" << span.column << "\n";

    auto lineText = source.getLine(span.line);
    std::string lineNumStr = std::to_string(span.line);
    std::string pad(lineNumStr.size(), ' ');

    os << " " << pad << " |\n";
    os << " " << lineNumStr << " | ";
    for (char16_t c : lineText) writeChar16(os, c);
    os << "\n";

    os << " " << pad << " | ";
    int col = span.column;
    for (int i = 0; i < col - 1; ++i) {
        char16_t ch = i < static_cast<int>(lineText.size()) ? lineText[i] : u' ';
        os << (ch == u'\t' ? '\t' : ' ');
    }
    int len = span.length > 0 ? span.length : 1;
    for (int i = 0; i < len; ++i) os << '^';
    os << "\n";
}
