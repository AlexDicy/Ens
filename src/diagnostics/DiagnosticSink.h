#pragma once
#include <ostream>
#include <string>
#include <vector>
#include "Diagnostic.h"

class SourceFile;

class DiagnosticSink {
public:
    void report(DiagnosticLevel level, SourceSpan span, std::string message);
    void error(SourceSpan span, std::string message);
    void warning(SourceSpan span, std::string message);

    bool empty() const { return diagnostics.empty(); }
    bool hasErrors() const;
    const std::vector<Diagnostic>& list() const { return diagnostics; }

    void printAll(const SourceFile& source, std::ostream& os) const;

private:
    std::vector<Diagnostic> diagnostics;
};
