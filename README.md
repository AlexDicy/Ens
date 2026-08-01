# Ens

Ens is a compiled programming language with automatic reference counting, checked exceptions, flow-sensitive nullability, and per-instantiation generics.

## Building and testing

```pwsh
xmake build ens-ref
xmake build ens-lld
xmake test
```

`ens-lld` is the linker bridge every Ens program links through.
`xmake test` runs the whole suite through the seed `ens` committed for this platform under `seed/`.

## Editor support

- `tools/vscode/` - VS Code extension (TextMate highlighting + `ens-lsp`).
- `tools/intellij/` - IntelliJ plugin (TextMate highlighting + `ens-lsp` via LSP4IJ).
- `tools/grammar/` - canonical TextMate grammar and language configuration shared by both.
