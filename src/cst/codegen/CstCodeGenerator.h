#pragma once
#include <memory>
#include <ostream>
#include <string>
#include <vector>
#include "../../diagnostics/Diagnostic.h"
#include "../../diagnostics/SourceFile.h"
#include "../SyntaxNode.h"
#include "../semantic/AnalysisResult.h"

class CstCodeGenerator {
public:
    CstCodeGenerator(std::string moduleName,
                     std::string sourceFilename,
                     const SourceFile& sourceFile,
                     const cst::semantic::AnalysisResult& analysis);
    ~CstCodeGenerator();

    bool generate(const SyntaxNode& sourceFileRoot);
    void print(std::ostream& os) const;

    bool emitObjectFile(const std::string& path);

    bool hasErrors() const;
    const std::vector<Diagnostic>& getDiagnostics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
