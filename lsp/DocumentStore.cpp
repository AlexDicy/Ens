#include "DocumentStore.h"

#include "parser/Parser.h"

Document::Document(std::string uri, std::u16string text, int version)
    : source(std::move(uri), std::move(text)), v(version) {
    parseAndAnalyze();
}

void Document::replaceText(std::u16string text, int version) {
    std::string keepUri = source.getFilename();
    source = SourceFile(std::move(keepUri), std::move(text));
    v = version;
    diagSink = DiagnosticSink();
    parseAndAnalyze();
}

void Document::parseAndAnalyze() {
    Parser parser(source.getSource(), diagSink);
    cstRoot = parser.parseSourceFile();
    rootNode = SyntaxNode::makeRoot(cstRoot.get());
    analyzerPtr = std::make_unique<Analyzer>(source, diagSink);
    analyzerPtr->analyze(*rootNode);
}

Document& DocumentStore::upsert(std::string uri, std::u16string text, int version) {
    auto it = docs.find(uri);
    if (it != docs.end()) {
        it->second->replaceText(std::move(text), version);
        return *it->second;
    }
    auto doc = std::make_unique<Document>(uri, std::move(text), version);
    auto* raw = doc.get();
    docs.emplace(std::move(uri), std::move(doc));
    return *raw;
}

void DocumentStore::erase(const std::string& uri) {
    docs.erase(uri);
}

Document* DocumentStore::find(const std::string& uri) {
    auto it = docs.find(uri);
    return it == docs.end() ? nullptr : it->second.get();
}

const Document* DocumentStore::find(const std::string& uri) const {
    auto it = docs.find(uri);
    return it == docs.end() ? nullptr : it->second.get();
}
