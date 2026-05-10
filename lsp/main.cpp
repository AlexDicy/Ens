#include <lsp/connection.h>
#include <lsp/io/standardio.h>
#include <lsp/messagehandler.h>
#include <lsp/messages.h>

#include "cst/SyntaxNode.h"
#include "diagnostics/DiagnosticSink.h"
#include "diagnostics/SourceFile.h"
#include "parser/Parser.h"
#include "semantic/Analyzer.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

std::u16string utf8To16(std::string_view s) {
    std::u16string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char16_t>(c));
            i += 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            out.push_back(static_cast<char16_t>(((c & 0x1F) << 6) | (c1 & 0x3F)));
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
            out.push_back(static_cast<char16_t>(
                ((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F)));
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
            unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
            unsigned char c3 = static_cast<unsigned char>(s[i + 3]);
            uint32_t cp = ((c & 0x07) << 18) | ((c1 & 0x3F) << 12) |
                          ((c2 & 0x3F) << 6) | (c3 & 0x3F);
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 | (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 | (cp & 0x3FF)));
            i += 4;
        } else {
            out.push_back(u'?');
            i += 1;
        }
    }
    return out;
}

struct Document {
    std::u16string text;
    int version = 0;
};

class EnsLanguageServer {
public:
    explicit EnsLanguageServer(lsp::MessageHandler& mh) : messages(mh) {}

    void onDidOpen(std::string uri, std::string_view text, int version) {
        auto& doc = documents[uri];
        doc.text = utf8To16(text);
        doc.version = version;
        publishDiagnostics(uri);
    }

    void onDidChange(std::string uri, std::string_view text, int version) {
        auto& doc = documents[uri];
        doc.text = utf8To16(text);
        doc.version = version;
        publishDiagnostics(uri);
    }

    void onDidClose(const std::string& uri) {
        documents.erase(uri);
    }

private:
    lsp::MessageHandler& messages;
    std::unordered_map<std::string, Document> documents;

    void publishDiagnostics(const std::string& uri) {
        auto it = documents.find(uri);
        if (it == documents.end()) return;
        const auto& doc = it->second;

        SourceFile sf(uri, doc.text);
        DiagnosticSink sink;
        Parser parser(sf.getSource(), sink);
        auto root = parser.parseSourceFile();
        auto rootNode = SyntaxNode::makeRoot(root.get());
        Analyzer analyzer(sf, sink);
        analyzer.analyze(*rootNode);

        std::vector<lsp::Diagnostic> diags;
        diags.reserve(sink.list().size());
        for (const auto& d : sink.list()) {
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
            diags.push_back(std::move(ld));
        }

        lsp::notifications::TextDocument_PublishDiagnostics::Params p;
        p.uri = lsp::Uri::parse(uri);
        p.version = doc.version;
        p.diagnostics = std::move(diags);
        messages.sendNotification<lsp::notifications::TextDocument_PublishDiagnostics>(std::move(p));
    }
};

}  // namespace

int main() {
    auto connection = lsp::Connection(lsp::io::standardIO());
    lsp::MessageHandler messages(connection);
    EnsLanguageServer server(messages);

    bool running = true;

    messages.add<lsp::requests::Initialize>(
        [](lsp::requests::Initialize::Params&&) {
            lsp::InitializeResult r;

            lsp::InitializeResultServerInfo info;
            info.name = "ens-lsp";
            info.version = std::string("0.1");
            r.serverInfo = std::move(info);

            lsp::ServerCapabilities caps;
            caps.positionEncoding =
                lsp::PositionEncodingKindEnum(lsp::PositionEncodingKind::UTF16);

            lsp::TextDocumentSyncOptions sync;
            sync.openClose = true;
            sync.change = lsp::TextDocumentSyncKindEnum(lsp::TextDocumentSyncKind::Full);
            caps.textDocumentSync = std::move(sync);

            r.capabilities = std::move(caps);
            return r;
        });

    messages.add<lsp::notifications::Initialized>([](auto&&) {});

    messages.add<lsp::notifications::TextDocument_DidOpen>(
        [&](lsp::notifications::TextDocument_DidOpen::Params&& p) {
            server.onDidOpen(p.textDocument.uri.toString(),
                             p.textDocument.text,
                             p.textDocument.version);
        });

    messages.add<lsp::notifications::TextDocument_DidChange>(
        [&](lsp::notifications::TextDocument_DidChange::Params&& p) {
            if (p.contentChanges.empty()) return;
            const auto& last = p.contentChanges.back();
            if (auto* full = std::get_if<lsp::TextDocumentContentChangeEvent_Text>(&last)) {
                server.onDidChange(p.textDocument.uri.toString(),
                                   full->text,
                                   p.textDocument.version);
            }
        });

    messages.add<lsp::notifications::TextDocument_DidClose>(
        [&](lsp::notifications::TextDocument_DidClose::Params&& p) {
            server.onDidClose(p.textDocument.uri.toString());
        });

    messages.add<lsp::requests::Shutdown>(
        []() { return lsp::ShutdownResult{}; });

    messages.add<lsp::notifications::Exit>(
        [&]() { running = false; });

    while (running) {
        messages.processIncomingMessages();
    }
    return 0;
}
