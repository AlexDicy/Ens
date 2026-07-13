#pragma once
#include <functional>
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
    // The analysis and source of some module in the compilation, looked up by
    // its canonical module path. Codegen uses this to resolve a node against the
    // module that owns it (e.g. a struct's field defaults reached through a
    // generic instantiation declared in another module). Both null when unknown.
    struct ModuleAnalysis {
        const AnalysisResult* analysis = nullptr;
        const SourceFile* source = nullptr;
    };
    using ModuleResolver = std::function<ModuleAnalysis(const std::u16string& modulePath)>;

    CodeGenerator(std::string moduleName,
                  std::string sourceFilename,
                  const SourceFile& sourceFile,
                  const AnalysisResult& analysis,
                  std::u16string modulePath = u"",
                  std::string targetTriple = "",
                  TypeContext* typeContext = nullptr,
                  ModuleResolver moduleResolver = {});
    ~CodeGenerator();

    bool generate(const SyntaxNode& sourceFileRoot);
    void print(std::ostream& os) const;

    bool emitObjectFile(const std::string& path);

    bool hasErrors() const;
    const std::vector<Diagnostic>& getDiagnostics() const;

    // Print each diagnostic against the source of the module that produced it.
    // A diagnostic raised while emitting a foreign module's node renders against
    // that module's file rather than the one currently being generated.
    void printDiagnostics(std::ostream& os) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
