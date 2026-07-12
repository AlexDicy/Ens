# Ens IntelliJ plugin (development)

Plugin for IntelliJ-based IDEs (including Community Edition) that highlights `.ens` files and spawns `ens-lsp` as the language server.
Syntax highlighting uses the canonical TextMate grammar and language configuration from `tools/grammar/`.
Both files are copied into the plugin resources at build time by `processResources` and are never committed under this folder.
Language features (diagnostics, hover, completion, go to definition, structure view, semantic tokens) come from `ens-lsp` through the [LSP4IJ](https://plugins.jetbrains.com/plugin/23257-lsp4ij) plugin.

## One-time setup

Build the language server:

```pwsh
xmake build ens-lsp
```

A JDK is not required up front.
Gradle provisions a Java 21 toolchain automatically on the first build.

## Run the plugin in a sandbox IDE

```pwsh
cd tools/intellij
./gradlew runIde
```

The sandbox IDE starts with the plugin and LSP4IJ installed.
Open this repository as a project and open a `.ens` file.
The server status and JSON-RPC traces are in **View | Tool Windows | Language Servers**.

## Package and install into a real IDE

```pwsh
./gradlew buildPlugin
```

This produces `build/distributions/ens-intellij-<version>.zip`.
Install LSP4IJ from the JetBrains Marketplace first; installing from disk does not resolve plugin dependencies.
Then use **Settings | Plugins | gear icon | Install Plugin from Disk** and pick the zip.

## Settings

**Settings | Languages & Frameworks | Ens** has one option: the path to the `ens-lsp` executable.
When empty, the newest binary under the project's `build/<os>/<arch>/<mode>/` tree is used, matching the VSCode extension.
