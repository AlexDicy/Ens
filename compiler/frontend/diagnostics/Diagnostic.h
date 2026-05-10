#pragma once
#include <ostream>
#include <stdexcept>
#include <string>
#include "SourceFile.h"

enum class DiagnosticLevel { Error, Warning, Note };

struct SourceSpan {
    int line;
    int column;
    int length;
};

class Diagnostic : public std::exception {
public:
    Diagnostic(DiagnosticLevel level, SourceSpan span, std::string message);

    const char* what() const noexcept override { return message.c_str(); }

    DiagnosticLevel getLevel() const { return level; }
    const SourceSpan& getSpan() const { return span; }
    const std::string& getMessage() const { return message; }

    void print(const SourceFile& source, std::ostream& os) const;

private:
    DiagnosticLevel level;
    SourceSpan span;
    std::string message;
};
