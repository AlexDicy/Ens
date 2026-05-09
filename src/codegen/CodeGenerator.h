#pragma once
#include <memory>
#include <ostream>
#include <string>
#include <vector>
#include "../ast/Stmt.h"
#include "../diagnostics/Diagnostic.h"

class CodeGenerator {
public:
    explicit CodeGenerator(std::string moduleName, std::string sourceFilename);
    ~CodeGenerator();

    bool generate(const std::vector<StmtPtr>& program);
    void print(std::ostream& os) const;

    bool emitObjectFile(const std::string& path);

    bool hasErrors() const;
    const std::vector<Diagnostic>& getDiagnostics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
