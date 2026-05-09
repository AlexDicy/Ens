#include "Compiler.h"
#include "../tokenizer/Tokenizer.h"
#include "../parser/Parser.h"
#include "../semantic/Analyzer.h"
#include "../codegen/CodeGenerator.h"
#include "../codegen/Linker.h"
#include "../cst/CstParser.h"
#include "../cst/SyntaxNode.h"
#include "../diagnostics/Diagnostic.h"
#include "../diagnostics/DiagnosticSink.h"
#include "../diagnostics/SourceFile.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>

namespace fs = std::filesystem;

bool Compiler::compile(const fs::path& source,
                       const fs::path& outputFolder,
                       const fs::path& sourcePath) {
    if (fs::is_directory(source)) {
        auto files = getFileTree(source, sourcePath);
        for (const auto& file : files) {
            compileSingle(source, file, outputFolder);
        }
    } else {
        return compileSingle(std::nullopt, source, outputFolder);
    }
    return true;
}

std::vector<fs::path> Compiler::getFileTree(const fs::path& root, const fs::path& rootPath) {
    std::vector<fs::path> files;

    auto rel = fs::relative(root, rootPath);
    bool hasSubfolder = !rel.empty() && rel != fs::path(".");

    for (const auto& entry : fs::directory_iterator(root)) {
        const auto& path = entry.path();
        if (fs::is_directory(path)) {
            auto sub = getFileTree(path, rootPath);
            files.insert(files.end(), sub.begin(), sub.end());
        } else if (fs::is_regular_file(path) && path.extension() == ".ens") {
            files.push_back(hasSubfolder ? rel / path.filename() : path.filename());
        }
    }

    // Make sure subfiles are last
    std::reverse(files.begin(), files.end());
    return files;
}

bool Compiler::compileSingle(const std::optional<fs::path>& root,
                              const fs::path& source,
                              const fs::path& outputFolder) {
    fs::path filePath = root.has_value() ? (*root / source) : source;
    std::ifstream file(filePath);
    if (!file) {
        std::cerr << "ERROR: Couldn't read " << source << '\n';
        return false;
    }

    return compileSingle(file, outputFolder, filePath.string());
}

bool Compiler::dumpCst(std::istream& source, const std::string& filename) {
    std::string code((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    std::u16string u16code(code.begin(), code.end());
    SourceFile sourceFile(filename, std::move(u16code));

    DiagnosticSink sink;
    CstParser parser(sourceFile.getSource(), sink);
    auto root = parser.parseSourceFile();

    auto rootNode = SyntaxNode::makeRoot(root.get());
    rootNode->dump(std::cout, 0);

    if (!sink.empty()) {
        std::cerr << "\n--- Diagnostics ---\n";
        sink.printAll(sourceFile, std::cerr);
    }
    return !sink.hasErrors();
}

bool Compiler::compileSingle(std::istream& source, const fs::path& outputFile, const std::string& filename) {
    std::string code((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    std::u16string u16code(code.begin(), code.end());
    SourceFile sourceFile(filename, std::move(u16code));

    try {
        auto tokens = Tokenizer::tokenize(sourceFile.getSource());
        Parser parser(std::move(tokens));
        auto stmts = parser.parseProgram();

        Analyzer analyzer;
        analyzer.analyze(stmts);
        if (analyzer.hasErrors()) {
            for (const auto& d : analyzer.getDiagnostics()) {
                d.print(sourceFile, std::cerr);
            }
            return false;
        }

        // Pick output mode from --output extension; default is IR to stdout.
        std::string ext = outputFile.extension().string();
        for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        const bool linkToExe = !outputFile.empty() && (ext == ".exe" || ext.empty());

        // For exe output, emit object to a sibling .obj path, then drop codegen
        // BEFORE invoking the linker.
        fs::path objPath;
        if (linkToExe) {
            objPath = outputFile;
            objPath.replace_extension(".obj");
        }

        {
            CodeGenerator codegen("ens_module", filename);
            if (!codegen.generate(stmts)) {
                for (const auto& d : codegen.getDiagnostics()) d.print(sourceFile, std::cerr);
                return false;
            }

            if (outputFile.empty()) {
                std::cout << "--- LLVM IR ---\n";
                codegen.print(std::cout);
                return true;
            }
            if (ext == ".ll") {
                std::ofstream out(outputFile);
                if (!out) {
                    std::cerr << "Could not open '" << outputFile << "' for writing\n";
                    return false;
                }
                codegen.print(out);
                return true;
            }
            if (ext == ".obj" || ext == ".o") {
                if (!codegen.emitObjectFile(outputFile.string())) {
                    for (const auto& d : codegen.getDiagnostics()) d.print(sourceFile, std::cerr);
                    return false;
                }
                return true;
            }
            if (linkToExe) {
                if (!codegen.emitObjectFile(objPath.string())) {
                    for (const auto& d : codegen.getDiagnostics()) d.print(sourceFile, std::cerr);
                    return false;
                }
            } else {
                std::cerr << "Unsupported --output extension: '" << ext << "'\n";
                return false;
            }
        }  // codegen LLVM context destroyed

        if (linkToExe) {
            if (!Linker::link(objPath.string(), outputFile.string(), std::cerr)) {
                return false;
            }
            return true;
        }
        return true;
    } catch (const Diagnostic& d) {
        d.print(sourceFile, std::cerr);
        return false;
    }
}
