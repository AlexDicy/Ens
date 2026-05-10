const path = require('path');
const fs = require('fs');
const vscode = require('vscode');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

let client;

function resolveServerPath(context) {
    const config = vscode.workspace.getConfiguration('ens');
    const configured = config.get('serverPath');
    if (configured && configured.length > 0) return configured;

    const exeName = process.platform === 'win32' ? 'ens-lsp.exe' : 'ens-lsp';
    const repoRoot = path.resolve(context.extensionPath, '..', '..');
    const candidates = [
        path.join(repoRoot, 'build', 'windows', 'x64', 'debug', exeName),
        path.join(repoRoot, 'build', 'windows', 'x64', 'release', exeName),
        path.join(repoRoot, 'build', 'linux', 'x86_64', 'debug', exeName),
        path.join(repoRoot, 'build', 'macosx', 'arm64', 'debug', exeName),
    ];
    for (const c of candidates) {
        if (fs.existsSync(c)) return c;
    }
    return candidates[0];
}

function activate(context) {
    const serverPath = resolveServerPath(context);
    if (!fs.existsSync(serverPath)) {
        vscode.window.showErrorMessage(
            `ens-lsp not found at ${serverPath}. Run \`xmake build ens-lsp\` or set "ens.serverPath".`);
        return;
    }

    const serverOptions = {
        run:   { command: serverPath, transport: TransportKind.stdio },
        debug: { command: serverPath, transport: TransportKind.stdio }
    };

    const clientOptions = {
        documentSelector: [{ scheme: 'file', language: 'ens' }],
    };

    client = new LanguageClient('ens', 'Ens Language Server', serverOptions, clientOptions);
    client.start();
    context.subscriptions.push({ dispose: () => client && client.stop() });
}

function deactivate() {
    if (client) return client.stop();
}

module.exports = { activate, deactivate };
