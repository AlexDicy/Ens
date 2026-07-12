#pragma once
#include <lsp/messagehandler.h>
#include <lsp/messages.h>

#include "DocumentStore.h"

class LanguageServer {
public:
    explicit LanguageServer(lsp::MessageHandler& mh);

    lsp::InitializeResult onInitialize(lsp::InitializeParams&& params);
    void onInitialized();

    void onDidOpen(lsp::notifications::TextDocument_DidOpen::Params&& p);
    void onDidChange(lsp::notifications::TextDocument_DidChange::Params&& p);
    void onDidClose(lsp::notifications::TextDocument_DidClose::Params&& p);
    void onDidChangeWatchedFiles(lsp::notifications::Workspace_DidChangeWatchedFiles::Params&& p);

    lsp::TextDocument_HoverResult onHover(lsp::HoverParams&& p);
    lsp::TextDocument_DefinitionResult onDefinition(lsp::DefinitionParams&& p);
    lsp::TextDocument_DocumentSymbolResult onDocumentSymbol(lsp::DocumentSymbolParams&& p);
    lsp::TextDocument_SemanticTokens_FullResult onSemanticTokensFull(lsp::SemanticTokensParams&& p);
    lsp::TextDocument_CompletionResult onCompletion(lsp::CompletionParams&& p);
    lsp::TextDocument_ReferencesResult onReferences(lsp::ReferenceParams&& p);
    lsp::TextDocument_PrepareRenameResult onPrepareRename(lsp::PrepareRenameParams&& p);
    lsp::TextDocument_RenameResult onRename(lsp::RenameParams&& p);

private:
    lsp::MessageHandler& messages;
    DocumentStore documents;

    void publishDiagnostics(const Document& doc);
    void refreshDocumentsDependingOn(const std::vector<std::filesystem::path>& changedPaths,
                                     const Document* skip);
};
