#pragma once
#include <vector>

#include "diagnostics/Diagnostic.h"
#include <lsp/messages.h>

std::vector<lsp::Diagnostic> toLspDiagnostics(const std::vector<Diagnostic>& diagnostics);
