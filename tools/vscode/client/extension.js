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
    const buildRoot = path.join(repoRoot, 'build');
    const platforms = [['windows', 'x64'], ['linux', 'x86_64'],
                       ['macosx', 'arm64'], ['macosx', 'x86_64']];
    const candidates = [];
    for (const [os, arch] of platforms) {
        for (const mode of ['release', 'debug']) {
            candidates.push(path.join(buildRoot, os, arch, mode, exeName));
        }
        // xmake may also output directly under the arch dir (no mode subfolder).
        candidates.push(path.join(buildRoot, os, arch, exeName));
    }

    // Pick the most recently built binary that exists, so a fresh build in either mode
    // wins over a stale one left behind in the other.
    let best = null;
    let bestMtime = -1;
    for (const c of candidates) {
        try {
            const mtime = fs.statSync(c).mtimeMs;
            if (mtime > bestMtime) { best = c; bestMtime = mtime; }
        } catch (_) { /* not built for this platform/mode */ }
    }
    return best || candidates[0];
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
