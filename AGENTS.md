Guidance for agents and new contributors working on the Ens language.
Read this before touching code; it encodes conventions that are enforced by review and by the test harness.

- [What this project is](#what-this-project-is)
- [Repository map](#repository-map)
- [Building and testing](#building-and-testing)
- [The bootstrap seed](#the-bootstrap-seed)
- [The self-hosted compiler](#the-self-hosted-compiler)
- [Diagnostics](#diagnostics)
- [Code style](#code-style)
- [Process](#process)

## What this project is

Ens is a compiled programming language with automatic reference counting, checked exceptions, flow-sensitive nullability, and per-instantiation generics.

Two compilers live in this repository, and knowing which is which matters before you change anything:

- **`ens`** is the compiler and build tool, written in Ens, under `selfhost/`.
  It is the whole toolchain: front end, semantic analyzer, LLVM code generator, linker, package resolution, and command-line interface.
  It compiles itself to a byte-identical fixpoint, which the `bootstrap` job gates on every test run.
  This is the `ens` command a user runs, and new language and tool work belongs here.
- **`ens-ref`** is the older compiler, written in C++, under `compiler/`.
  It is no longer the product.
  It stays for two reasons: it compiles the `tests/` fixtures as a second opinion, and it drives five CLI jobs that will be deleted along with it.
  Nothing blocks its removal any more; the seed a fresh clone bootstraps from is committed, not built (see [The bootstrap seed](#the-bootstrap-seed)).

`spec.md` is the single source of truth for user-facing language and tool behavior.
If the spec, `ens`, and `ens-ref` disagree, that is a bug worth surfacing, not a detail to paper over.

## Repository map

- `compiler/` - the C++ reference compiler behind `ens-ref`: front end (lexer/parser/CST), semantic analyzer, LLVM codegen, module/workspace resolution, and its own driver.
- `libs/std/` - the standard library, written in Ens, imported as the built-in `@std` package.
- `runtime/lld/` - `ens_lld.cpp`, one C entry point over lld's C++ link drivers, built as the `ens-lld` shared library. It carries no link policy and is meant to outlive `compiler/`.
- `selfhost/` - the `ens` compiler and build tool, written in Ens, as one workspace of packages:
  - `syntax.grammar` + `syntaxgen/` - the declarative grammar and the generator that emits the syntax-tree sources.
  - `frontend/` - lexer, event parser, red-green syntax tree.
  - `sema/` - the semantic layer: module graph, types, declarations, signatures, layout, generics, expression/statement checking, flow narrowing, checked exceptions.
  - `llvm/` - the binding over LLVM-C: contexts, modules, builders, types, values, target machines.
  - `codegen/` - lowering to EIR (the ownership-explicit intermediate form), its verifiers, the optimization passes, and LLVM emission.
  - `host/` - what a run reads from the machine once: the environment snapshot, the user's `.ens` folder, path text, and a scratch folder that removes itself.
  - `cli/` - the command-line library: the immutable declaration model, the pure argv walk, generated help, nearest-match suggestions.
  - `link/` - the `ens-lld` binding plus all link policy: flavor from triple, per-flavor argument vector, C runtime, unwinder and SDK discovery.
  - `packages/` - dependency resolution: `ens.overrides`, `ens.lock`, the content store, sha256, git fetching, prebuilt artifacts, version selection.
  - `build/` - target resolution, the toolchain decision, build and check orchestration, test discovery and runner synthesis, reporting.
  - `driver/` - the `ens` executable itself: command declarations, dispatch, exit codes.
  - `corpus/` - a harness asserting the front end parses every `.ens` file in the repo losslessly.
  - `semacheck/` - the differential harness gating sema behavior against `ens-ref` (see Test conventions).
  - `codegencheck/` - the differential harness gating code generation, with `codegencheck/spike` as its own member: a small program that emits an object file through the LLVM binding.
- `tests/` - the compiler test suite; every fixture is executable specification.
- `lsp/`, `tools/` - editor integrations; `tools/grammar/` is canonical for the shared grammar/config files.
- `scripts/xmake_test.lua` - the test runner; read its header comment for the fixture directives.

## Building and testing

- Two native targets carry the compiler and its tests: `xmake build ens-ref` and `xmake build ens-lld`.
  `ens-lld` is the linker bridge every Ens program links through, so a fresh clone cannot reach an executable without it.
  `ens-lsp` is for editor work only and is built on its own (see `tools/vscode/README.md`).
  Do not build all targets routinely; `ens-lsp.exe` may be running and will fail to relink.
- Run everything: `xmake test` (subset: `xmake test <name>...`).
  The full suite must be green before every commit, with no exceptions.
  `xmake test` builds either of those targets itself when it is missing, so it is the one command that always works from a clean checkout.
- `xmake test` starts by placing the **seed** and then shares it: the committed `ens` for this host is copied into `build/seed/ens`, and every job that compiles Ens uses that seed.
  Everything Ens-related is therefore built by the Ens compiler, so a bug that lives only in `ens-ref` cannot shape the self-hosted tree.
  The seed is placed before the jobs start, because they run in parallel and all of them need it.
- `ens-ref`'s remaining roles are exactly two: it compiles the `tests/` fixtures for the reference gate, and it drives `cli_core`, `cli_workspace`, `cli_override`, `cli_git` and `cli_artifact`.
  Those five jobs have `ens`-side counterparts already, so they are deleted with the C++ driver rather than ported.
- The packaging tests (`cli_git`, `cli_artifact`, `cli_dependencies`, `cli_prebuilt`) shell out to the system `git` and `curl`; both must be on PATH.
  They drive scratch repositories over `file://` URLs and a scratch cache, so no test ever reaches the network or this machine's own cache.
- `xmake test` uses the binary of the currently configured mode.
  Never run `xmake f -m release` to "fix" staleness; rebuild instead.
  If linking fails with unresolved LLVM symbols, put the LLVM package's `bin` folder (containing `clang++.exe`) on PATH, run `xmake f -c -m <current mode> -p windows -a x64`, then `xmake build ens-ref`.
  Pass the platform and architecture explicitly: with `clang++` on PATH and no `-p`/`-a`, xmake detects `mingw/x86_64` and then refuses the packages.
  Building without that `bin` folder on PATH re-resolves the linker to MSVC `link.exe`, which silently ignores `-lLLVMCGData` and leaves unresolved LLVM CGData symbols.
  That choice is written into the config, so later builds keep failing even once PATH is fixed until the reconfigure above is rerun.
- Run only one build/test session at a time; concurrent xmake invocations collide.

### Test conventions

- A fixture is a `tests/*.ens` file, a `tests/<dir>/main.ens` folder program, or a `tests/<dir>/src/main.ens` package.
  Header directives drive assertions: `// @exit N`, `// @stdout ...`, `// @expect-error <substring>`, `// @ens-test <args>`.
  A fixture carrying `@ens-test` runs through the seed `ens`; every other fixture is compiled by `ens-ref`, and `codegencheck` compiles all of them through the self-hosted pipeline as well.
- Group related scenarios into one or two files (happy paths vs errors), not one file per scenario.
- Unit test coverage matters: new code ships with tests for its own logic (the self-hosted packages keep unit tests in their `tests/` folders), not just end-to-end fixtures.
- A package's `tests/` folder mirrors the grouping of its `src/` folder; put new tests in the subfolder matching the code under test.
- `corpus_roundtrip` asserts the self-hosted front end parses the whole repo byte-exact with no unexpected diagnostics.
- `semacheck` is a bidirectional gate: every accepted program must produce zero self-hosted sema diagnostics, and every `@expect-error` fixture must be rejected by sema.
  A fixture whose diagnostic belongs to a later phase carries `// @expect-error-at <phase>`; a tag sema outgrows fails the run as stale.
  Consequence: new language behavior must land in `ens-ref` and in the self-hosted sema in lockstep (atomic commits are acceptable when needed to keep every commit green).
- `codegencheck` runs twice, once per shipped code-generation configuration: `codegencheck` at `-O2` and `codegencheck_unoptimized` at `-O0`.
  `-O0` is what every program built with `-O0` gets, so it is gated exactly as hard as the default is.
- Both arms must keep `selfhost/codegencheck/skiplist.txt` **empty**.
  The list may only shrink: the harness fails if a runnable fixture is neither verified nor listed, and equally if a listed name is no longer a runnable fixture, so the exemption list cannot rot.
- `bootstrap` is the strongest end-to-end gate: the seed compiles `selfhost/driver`, the `ens` that came out of that compiles the same sources again, and the two stages' object files must hold the same modules with identical bytes.
  Executables are compared as a note only, because linker output carries timestamps.
- The `cli_*` jobs split by which binary they drive.
  `cli_core`, `cli_workspace`, `cli_override`, `cli_git` and `cli_artifact` drive `ens-ref`; `cli_build`, `cli_runtest`, `cli_overriding`, `cli_dependencies`, `cli_prebuilt` and `cli_toolchain` drive `ens`.
  The two command lines are deliberately different, so there is no scenario-for-scenario differential gate between them: each job asserts its own binary's behavior.

## The bootstrap seed

`ens` is written in Ens, so building it needs an `ens` that already runs.
That first one is committed to the repository, one binary per host, and `xmake test` copies the one matching the host into `build/seed/` before any job starts:

- `seed/windows-x64/ens.exe`
- `seed/linux-x64/ens`
- `seed/macos-arm64/ens` is where a macOS seed goes; there is none yet.

A host with no committed seed fails the run by name instead of falling back to another compiler, because a fallback would leave the seed itself untested.
So a fresh clone needs no compiler built from C++ to compile Ens, only the `ens-lld` linker bridge and the LLVM package every build links against.

Refreshing a seed is a deliberate act rather than a build artifact.
When a language or library change makes the tree uncompilable by the committed seed, the new binary lands in the same commit as the source change, because every commit has to be buildable from its own checkout.
Build the replacement with an `ens` that already runs on that platform and commit it in place; a platform nobody can produce a seed for cannot bootstrap at all until somebody does.
The cost this model accepts is a multi-megabyte binary per platform in git history, growing every time it is refreshed, and a supply-chain question no reviewer can answer by reading a diff.

## The self-hosted compiler

- It is a clean redesign, not a port.
  `ens-ref` is authoritative for observable language behavior only (what is accepted, rejected, and diagnosed); never mirror its structure, naming, or style, and nothing about its command line is authoritative at all.
- Parity is the floor, not the ceiling: when work reveals a hole in `ens-ref` (a crash, unsound accept, weak diagnostic), surface it for triage instead of replicating it.
  Genuine bugs that block a design choice stop the work; report a minimal repro rather than adopting a workaround design.
- Generated files (`selfhost/frontend/src/syntax/generated/kind.ens`, `nodes.ens`, `factory.ens`, `dump.ens`) say "Generated by syntaxgen - Do not edit".
  Change `selfhost/syntax.grammar` or the emitter in `selfhost/syntaxgen/src/` and regenerate.
- Ens packages use a workspace model: an `ens.package` manifest declares either one package or a workspace of member packages, a package's `dependency` declarations resolve by name against the enclosing workspace's members, and a package is consumed through its `src/` folder.
  Keep knowledge of the manifest format isolated where it already lives (`compiler/frontend/module/` and `selfhost/sema/src/program/workspace.ens`).
- Per-file work (parsing, declaration scanning) must stay a pure function with no shared mutable state; cross-file phases are staged and return their results and diagnostics as values.
  Threads are coming to Ens eventually; do not add designs that would need refactoring then.

## Diagnostics

Diagnostics are the user experience of the language and are held to a high bar.

- User-facing messages use the real type/symbol names from the user's program, plain language a beginner can follow, a suggested fix, and where natural a concrete example of the correct form.
- Never copy a weak message from `ens-ref` into new code for parity; write the good version and flag the C++ side for triage.
- Messages prefixed `Internal:` are bug-catchers for states no valid program can reach; they may cite implementation details and are exempt from the wording bar.
  Use the prefix only when the error genuinely cannot reach a user.

## Code style

Ens code (selfhost, libs, tests):

- Expand all blocks: every body on multiple lines, even one-line accessors; blank line between members.
  Empty blocks may stay inline.
- Group related fields with no blank lines inside a group, one blank line between groups.
- Soft 100-column limit; prefer `switch` over if/else chains when dispatching on one value.
- Full words for identifiers; American English everywhere.
- No `get` prefix on a method that always returns a value: `stackTrace()`, not `getStackTrace()`.
  Keep the prefix only where the thing asked for may legitimately not be there, which the return type shows: `system.getEnvironmentVariable(name) -> string?`.
  The `ens.llvm` binding is exempt because its names mirror the C API it wraps.
- Programs and libraries split into `src/` plus a sibling `tests/` folder (`ens test <package folder>` runs them); library sources must not define `main()`.

C++ code: match the surrounding style of the file you are in.

Comments (both languages): minimal.
Prefer clean, understandable code over explaining unclear code with a comment.
A comment only states what the code cannot; never phase tags, decision rationale, or references to "the other compiler" - that context belongs in commits and reviews, not source.

## Process

- Commit messages start with a short imperative line.
- One commit per completed milestone or logical change; unrelated fixes discovered along the way go in their own commits.
- When opening a pull request, prefer small diffs a human can actually review over one huge drop; split large efforts into a sequence of reviewable changes.
- Spec changes are user-facing only and written one sentence per line.
- In reports and reviews, label behavioral claims as verified (you ran it) or inferred (you reasoned from code); do not present inference as established fact.
