#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "cst/Green.h"
#include "cst/SyntaxNode.h"
#include "diagnostics/DiagnosticSink.h"
#include "diagnostics/SourceFile.h"
#include "semantic/Analyzer.h"

class Document {
public:
    Document(std::string uri, std::u16string text, int version);

    void replaceText(std::u16string text, int version);

    const std::string& uri() const { return source.getFilename(); }
    int version() const { return v; }
    const SourceFile& sourceFile() const { return source; }
    const SyntaxNode& root() const { return *rootNode; }
    const Analyzer& analyzer() const { return *analyzerPtr; }
    Analyzer& analyzer() { return *analyzerPtr; }
    const DiagnosticSink& sink() const { return diagSink; }

private:
    SourceFile source;
    int v;
    DiagnosticSink diagSink;
    GreenElementPtr cstRoot;
    std::unique_ptr<SyntaxNode> rootNode;
    std::unique_ptr<Analyzer> analyzerPtr;

    void parseAndAnalyze();
};

class DocumentStore {
public:
    Document& upsert(std::string uri, std::u16string text, int version);
    void erase(const std::string& uri);
    Document* find(const std::string& uri);
    const Document* find(const std::string& uri) const;

private:
    std::unordered_map<std::string, std::unique_ptr<Document>> docs;
};
