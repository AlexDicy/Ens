#include <lsp/connection.h>
#include <lsp/io/standardio.h>
#include <lsp/messagehandler.h>
#include <lsp/messages.h>

#include "LanguageServer.h"

int main() {
    auto connection = lsp::Connection(lsp::io::standardIO());
    lsp::MessageHandler messages(connection);
    LanguageServer server(messages);

    bool running = true;

    messages.add<lsp::requests::Initialize>(
        [&](lsp::requests::Initialize::Params&& p) { return server.onInitialize(std::move(p)); });

    messages.add<lsp::notifications::Initialized>(
        [&](auto&&) { server.onInitialized(); });

    messages.add<lsp::notifications::TextDocument_DidOpen>(
        [&](lsp::notifications::TextDocument_DidOpen::Params&& p) {
            server.onDidOpen(std::move(p));
        });

    messages.add<lsp::notifications::TextDocument_DidChange>(
        [&](lsp::notifications::TextDocument_DidChange::Params&& p) {
            server.onDidChange(std::move(p));
        });

    messages.add<lsp::notifications::TextDocument_DidClose>(
        [&](lsp::notifications::TextDocument_DidClose::Params&& p) {
            server.onDidClose(std::move(p));
        });

    messages.add<lsp::requests::TextDocument_Hover>(
        [&](lsp::requests::TextDocument_Hover::Params&& p) { return server.onHover(std::move(p)); });

    messages.add<lsp::requests::TextDocument_Definition>(
        [&](lsp::requests::TextDocument_Definition::Params&& p) {
            return server.onDefinition(std::move(p));
        });

    messages.add<lsp::requests::TextDocument_DocumentSymbol>(
        [&](lsp::requests::TextDocument_DocumentSymbol::Params&& p) {
            return server.onDocumentSymbol(std::move(p));
        });

    messages.add<lsp::requests::TextDocument_SemanticTokens_Full>(
        [&](lsp::requests::TextDocument_SemanticTokens_Full::Params&& p) {
            return server.onSemanticTokensFull(std::move(p));
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
