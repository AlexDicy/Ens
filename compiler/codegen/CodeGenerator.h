#pragma once
#include <memory>
#include <ostream>
#include <string>
#include <vector>
#include "diagnostics/Diagnostic.h"
#include "diagnostics/SourceFile.h"
#include "cst/SyntaxNode.h"
#include "semantic/AnalysisResult.h"

class TypeContext;

class CodeGenerator {
public:
    CodeGenerator(std::string moduleName,
                  std::string sourceFilename,
                  const SourceFile& sourceFile,
                  const AnalysisResult& analysis,
                  std::u16string modulePath = u"",
                  std::string targetTriple = "",
                  TypeContext* typeContext = nullptr);
    ~CodeGenerator();

    bool generate(const SyntaxNode& sourceFileRoot);
    void print(std::ostream& os) const;

    bool emitObjectFile(const std::string& path);

    bool hasErrors() const;
    const std::vector<Diagnostic>& getDiagnostics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
