# Ens VS Code extension (development)

Tiny extension that registers `.ens` files and spawns `ens-lsp` as the language server.

## One-time setup

```pwsh
cd tools/vscode
npm install
```

Build the language server:

```pwsh
cd ../..
xmake build ens-lsp
```

## Run the extension in a dev host

1. Open the `tools/vscode` folder in VS Code.
2. Press `F5` (uses `.vscode/launch.json`).
3. The dev host opens. Open a `.ens` file in it (e.g. one of the project's `tests/*.ens`).
4. The status bar shows "Ens Language Server" once the server attaches.
5. Edit the file - diagnostics appear in real time (the server re-parses on each change).

## Settings

- `ens.serverPath` - override the path to `ens-lsp` if you've built it somewhere unusual. Defaults to `../../build/<plat>/<arch>/<mode>/ens-lsp[.exe]` relative to this folder.
- `ens.trace.server` - set to `verbose` and inspect the **Output → Ens Language Server** panel to see the JSON-RPC traffic.
