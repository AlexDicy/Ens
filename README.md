# Ens

Ens is a compiled programming language with automatic reference counting, checked exceptions, flow-sensitive nullability, and per-instantiation generics.
`spec.md` describes the language and the `ens` command.

## The two compilers

- `ens` - the compiler and build tool, written in Ens, under `selfhost/`. This is the command a user runs.
- `ens-ref` - the older C++ compiler, under `compiler/`. It builds the seed `ens` that a fresh clone bootstraps from and gates the `tests/` fixtures.

## Building and testing

```pwsh
xmake build ens-ref
xmake build ens-lld
xmake test
```

`ens-lld` is the linker bridge every Ens program links through.
`xmake test` builds the seed `ens` with `ens-ref` and then runs the whole suite through it.

`AGENTS.md` has the repository map, the conventions, and the build notes for when the above does not work.

## Editor support

- `tools/vscode/` - VS Code extension (TextMate highlighting + `ens-lsp`).
- `tools/intellij/` - IntelliJ plugin (TextMate highlighting + `ens-lsp` via LSP4IJ).
- `tools/grammar/` - canonical TextMate grammar and language configuration shared by both.
