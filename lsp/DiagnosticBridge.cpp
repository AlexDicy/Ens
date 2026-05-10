#include "DiagnosticBridge.h"

#include <algorithm>
#include <utility>

std::vector<lsp::Diagnostic> toLspDiagnostics(const std::vector<Diagnostic>& diagnostics) {
    std::vector<lsp::Diagnostic> out;
    out.reserve(diagnostics.size());
    for (const auto& d : diagnostics) {
        const auto& span = d.getSpan();
        int startLine = std::max(0, span.line - 1);
        int startCh = std::max(0, span.column - 1);
        int endCh = startCh + std::max(1, span.length);

        lsp::Diagnostic ld;
        ld.range.start.line = startLine;
        ld.range.start.character = startCh;
        ld.range.end.line = startLine;
        ld.range.end.character = endCh;

        lsp::DiagnosticSeverity sev = lsp::DiagnosticSeverity::Error;
        switch (d.getLevel()) {
            case DiagnosticLevel::Error:   sev = lsp::DiagnosticSeverity::Error; break;
            case DiagnosticLevel::Warning: sev = lsp::DiagnosticSeverity::Warning; break;
            case DiagnosticLevel::Note:    sev = lsp::DiagnosticSeverity::Information; break;
        }
        ld.severity = lsp::DiagnosticSeverityEnum(sev);
        ld.message = d.getMessage();
        ld.source = std::string("ens");
        out.push_back(std::move(ld));
    }
    return out;
}
