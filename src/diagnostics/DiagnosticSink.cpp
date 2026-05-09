#include "DiagnosticSink.h"
#include "SourceFile.h"

void DiagnosticSink::report(DiagnosticLevel level, SourceSpan span, std::string message) {
    diagnostics.emplace_back(level, span, std::move(message));
}

void DiagnosticSink::error(SourceSpan span, std::string message) {
    report(DiagnosticLevel::Error, span, std::move(message));
}

void DiagnosticSink::warning(SourceSpan span, std::string message) {
    report(DiagnosticLevel::Warning, span, std::move(message));
}

bool DiagnosticSink::hasErrors() const {
    for (const auto& d : diagnostics) {
        if (d.getLevel() == DiagnosticLevel::Error) return true;
    }
    return false;
}

void DiagnosticSink::printAll(const SourceFile& source, std::ostream& os) const {
    for (const auto& d : diagnostics) d.print(source, os);
}
