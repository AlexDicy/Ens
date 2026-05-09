#include "Compiler.h"
#include "../ast/Declaration.h"
#include "../codegen/CodeGenerator.h"
#include "../codegen/Linker.h"
#include "../cst/SyntaxNode.h"
#include "../diagnostics/Diagnostic.h"
#include "../diagnostics/DiagnosticSink.h"
#include "../diagnostics/SourceFile.h"
#include "../parser/Parser.h"
#include "../semantic/Analyzer.h"

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

static std::string asAscii16(std::u16string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char16_t c : s) out.push_back(c < 128 ? static_cast<char>(c) : '?');
    return out;
}

static void dumpTypedOutline(const SyntaxNode& root, std::ostream& os) {
    auto sf = ast::SourceFile::cast(root);
    if (!sf) return;
    os << "\n--- Typed outline ---\n";
    for (auto& fn : sf->functions()) {
        os << "fn " << (fn.nameText() ? asAscii16(*fn.nameText()) : std::string("<missing>")) << "(";
        bool first = true;
        for (auto& p : fn.parameters()) {
            if (!first) os << ", ";
            first = false;
            if (p.isThisField()) os << "this.";
            os << (p.nameText() ? asAscii16(*p.nameText()) : std::string("<?>"));
            if (auto tr = p.typeReference()) {
                os << ":" << (tr->nameText() ? asAscii16(*tr->nameText()) : std::string("<?>"));
                if (tr->isOptional()) os << "?";
            }
            if (p.defaultValue()) os << "=...";
        }
        os << ")";
        if (auto rt = fn.returnType()) {
            if (auto tr = rt->typeReference()) {
                os << " -> " << (tr->nameText() ? asAscii16(*tr->nameText()) : std::string("<?>"));
            }
        }
        if (fn.isShorthand()) os << " ;";
        os << "\n";
    }
    for (auto& sd : sf->structs()) {
        os << "struct " << (sd.nameText() ? asAscii16(*sd.nameText()) : std::string("<missing>")) << " {\n";
        for (auto& f : sd.fields()) {
            os << "  field " << (f.nameText() ? asAscii16(*f.nameText()) : std::string("<?>"));
            if (auto tr = f.typeReference()) os << " : " << (tr->nameText() ? asAscii16(*tr->nameText()) : std::string("<?>"));
            os << "\n";
        }
        for (auto& m : sd.methods()) {
            os << "  method " << (m.nameText() ? asAscii16(*m.nameText()) : std::string("<?>"))
               << "/" << m.parameters().size() << "\n";
        }
        os << "}\n";
    }
    for (auto& cd : sf->classes()) {
        os << "class " << (cd.nameText() ? asAscii16(*cd.nameText()) : std::string("<missing>")) << " {\n";
        for (auto& f : cd.fields()) {
            os << "  field " << (f.nameText() ? asAscii16(*f.nameText()) : std::string("<?>"));
            if (auto tr = f.typeReference()) os << " : " << (tr->nameText() ? asAscii16(*tr->nameText()) : std::string("<?>"));
            os << "\n";
        }
        for (auto& m : cd.methods()) {
            os << "  method " << (m.nameText() ? asAscii16(*m.nameText()) : std::string("<?>"))
               << "/" << m.parameters().size();
            if (m.isShorthand()) os << " (shorthand)";
            os << "\n";
        }
        os << "}\n";
    }
}

bool Compiler::dumpCst(std::istream& source, const std::string& filename) {
    std::string code((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    std::u16string u16code(code.begin(), code.end());
    SourceFile sourceFile(filename, std::move(u16code));

    DiagnosticSink sink;
    Parser parser(sourceFile.getSource(), sink);
    auto root = parser.parseSourceFile();

    auto rootNode = SyntaxNode::makeRoot(root.get());
    rootNode->dump(std::cout, 0);
    dumpTypedOutline(*rootNode, std::cout);

    if (!sink.empty()) {
        std::cerr << "\n--- Diagnostics ---\n";
        sink.printAll(sourceFile, std::cerr);
    }
    return !sink.hasErrors();
}

bool Compiler::analyzeCst(std::istream& source, const std::string& filename) {
    std::string code((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    std::u16string u16code(code.begin(), code.end());
    SourceFile sourceFile(filename, std::move(u16code));

    DiagnosticSink sink;
    Parser parser(sourceFile.getSource(), sink);
    auto root = parser.parseSourceFile();
    auto rootNode = SyntaxNode::makeRoot(root.get());

    Analyzer analyzer(sourceFile, sink);
    analyzer.analyze(*rootNode);

    if (!sink.empty()) {
        sink.printAll(sourceFile, std::cerr);
    }
    return !sink.hasErrors();
}

bool Compiler::compileSingle(std::istream& source, const fs::path& outputFile, const std::string& filename) {
    std::string code((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    std::u16string u16code(code.begin(), code.end());
    SourceFile sourceFile(filename, std::move(u16code));

    DiagnosticSink sink;
    Parser parser(sourceFile.getSource(), sink);
    auto root = parser.parseSourceFile();
    auto rootNode = SyntaxNode::makeRoot(root.get());

    Analyzer analyzer(sourceFile, sink);
    analyzer.analyze(*rootNode);

    if (sink.hasErrors()) {
        sink.printAll(sourceFile, std::cerr);
        return false;
    }

    std::string ext = outputFile.extension().string();
    for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    const bool linkToExe = !outputFile.empty() && (ext == ".exe" || ext.empty());

    fs::path objPath;
    if (linkToExe) {
        objPath = outputFile;
        objPath.replace_extension(".obj");
    }

    {
        CodeGenerator codegen("ens_module", filename, sourceFile, analyzer.result());
        if (!codegen.generate(*rootNode)) {
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
    }

    if (linkToExe) {
        if (!Linker::link(objPath.string(), outputFile.string(), std::cerr)) return false;
        return true;
    }
    return true;
}
