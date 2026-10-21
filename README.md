# Ens

Ens is a compiled programming language with automatic reference counting, checked exceptions, flow-sensitive nullability, and per-instantiation generics.

## Installing

Download `ens` for your machine from the [releases](https://github.com/AlexDicy/Ens/releases).

### Windows

The folder below is one Windows puts on `PATH`.

```cmd
move ens-windows-x64.exe "%LOCALAPPDATA%\Microsoft\WindowsApps\ens.exe"
```

### macOS

A file a browser downloaded is quarantined until the mark is removed.

```sh
chmod +x ens-macos-arm64
xattr -d com.apple.quarantine ens-macos-arm64
sudo mv ens-macos-arm64 /usr/local/bin/ens
```

### Linux

```sh
chmod +x ens-linux-x64
sudo mv ens-linux-x64 /usr/local/bin/ens
```

### Building your first program with Ens

Save this as `hello.ens`:

```ens
main() -> int {
    print("Hello, world!");
    return 0;
}
```

Build it into an executable named `hello`:

```sh
ens build hello.ens
```

Or build and run it in one step:

```sh
ens run hello.ens
```

`--target` builds for another machine, here a Linux one:

```sh
ens build hello.ens --target x86_64-unknown-linux-gnu
```

The first build writes the standard library and the files a link needs into `~/.ens/cache`.

## Building and testing

```pwsh
xmake build ens-llvm
xmake test
```

`ens-llvm` is the one static library `ens` links for LLVM and lld: the linker bridge every Ens program links through, together with LLVM itself, and the only C++ a build of Ens needs.
`xmake test` builds `ens` from source with the seed committed for this platform under `seed/`, then runs the whole suite through it.

## Editor support

- `tools/vscode/` - VS Code extension (TextMate highlighting + `ens-lsp`).
- `tools/intellij/` - IntelliJ plugin (TextMate highlighting + `ens-lsp` via LSP4IJ).
- `tools/grammar/` - canonical TextMate grammar and language configuration shared by both.
