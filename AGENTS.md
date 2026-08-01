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

`ens` is the compiler and build tool, written in Ens, under `selfhost/`.
It is the whole toolchain: front end, semantic analyzer, LLVM code generator, linker, package resolution, and command-line interface.
It compiles itself to a byte-identical fixpoint, which the `bootstrap` job gates on every test run.
It is the `ens` command a user runs, and it is the only compiler this repository has.

The C++ compiler Ens was first written in has been deleted.
Nothing in the tree answers for its behavior any more, and building, testing and bootstrapping Ens go through no C++ but the `ens-lld` linker bridge.
The language server is C++ as well, but it is a separate tool with a front end of its own; see the repository map.

`spec.md` is the single source of truth for user-facing language and tool behavior.
If the spec, `ens`, and the `tests/` fixtures disagree, that is a bug worth surfacing, not a detail to paper over.

## Repository map

- `libs/std/` - the standard library, written in Ens, imported as the built-in `@std` package.
- `runtime/lld/` - `ens_lld.cpp`, one C entry point over lld's C++ link drivers, built as the `ens-lld` shared library. It carries no link policy; that all lives in `selfhost/link/`.
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
  - `semacheck/` - the harness gating sema behavior against every fixture and package in the tree (see Test conventions).
  - `codegencheck/` - the harness gating code generation against every runnable fixture, with `codegencheck/spike` as its own member: a small program that emits an object file through the LLVM binding.
- `tests/` - the compiler test suite; every fixture is executable specification.
- `lsp/` - the language server, in C++, and the only C++ here besides `runtime/lld/`:
  - `server/` - the server itself, built as `ens-lsp`: the document store, the diagnostic bridge, and the requests it answers.
  - `frontend/` - the C++ lexer, parser, CST and semantic analyzer it reads a document with, built as `lsp-frontend`. It belongs to the language server and to nothing else; the language's own front end is `selfhost/frontend/`, and a question about the language is answered there or in `spec.md`, never here.
- `tools/` - editor integrations; `tools/grammar/` is canonical for the shared grammar/config files.
- `scripts/xmake_test.lua` - the test runner; read its header comment for the fixture directives.

## Building and testing

- One native target carries what the tests need: `xmake build ens-lld`, the linker bridge every Ens program links through, without which a fresh clone cannot reach an executable.
- The language server is built on its own: `xmake build ens-lsp` (see `tools/vscode/README.md`).
  Nothing in `xmake test` builds it, so the whole suite can stay green while the language server no longer compiles.
  Any change that touches `lsp/`, and any repository-wide rename or move that its sources could be reading, is gated by running `xmake build ens-lsp` by hand.
  Close a running `ens-lsp.exe` first, or the link fails on the file being in use.
- Run everything: `xmake test` (subset: `xmake test <name>...`).
  The full suite must be green before every commit, with no exceptions.
  `xmake test` builds `ens-lld` itself when it is missing, so it is the one command that always works from a clean checkout.
- `xmake test` starts by building the compiler it then tests: the committed **seed** for this host compiles `selfhost/driver` into `build/host/ens`, and every job that compiles Ens drives that.
  Everything Ens-related is therefore built by the Ens compiler as this tree defines it, so nothing frozen into the seed can shape what the suite measures.
  It is built before the jobs start, because they run in parallel and all of them need it.
- The packaging tests (`cli_dependencies`, `cli_prebuilt`) shell out to the system `git` and `curl`; both must be on PATH.
  They drive scratch repositories over `file://` URLs and a scratch cache, so no test ever reaches the network or this machine's own cache.
- `xmake test` uses the binary of the currently configured mode.
  Never run `xmake f -m release` to "fix" staleness; rebuild instead.
  If linking fails with unresolved LLVM symbols, put the LLVM package's `bin` folder (containing `clang++.exe`) on PATH, run `xmake f -c -m <current mode> -p windows -a x64`, then `xmake build ens-lld`.
  Pass the platform and architecture explicitly: with `clang++` on PATH and no `-p`/`-a`, xmake detects `mingw/x86_64` and then refuses the packages.
  Building without that `bin` folder on PATH re-resolves the linker to MSVC `link.exe`, which silently ignores `-lLLVMCGData` and leaves unresolved LLVM CGData symbols.
  That choice is written into the config, so later builds keep failing even once PATH is fixed until the reconfigure above is rerun.
- Run only one build/test session at a time; concurrent xmake invocations collide.

### Test conventions

- A fixture is a `tests/*.ens` file, a `tests/<dir>/main.ens` folder program, or a `tests/<dir>/src/main.ens` package.
  Header directives drive assertions: `// @exit N`, `// @stdout ...`, `// @expect-error <substring>`, `// @ens-test <args>`.
  A fixture carrying `@ens-test` runs through `ens test`; every other fixture is compiled by `ens build`, and `codegencheck` compiles all of them through the self-hosted pipeline as well.
  An `@expect-error` substring is the diagnostic's wording, so it is what holds the message to its bar: never shorten one to make a run pass, and never point one at a weaker message than the compiler can give.
- Group related scenarios into one or two files (happy paths vs errors), not one file per scenario.
- Unit test coverage matters: new code ships with tests for its own logic (the self-hosted packages keep unit tests in their `tests/` folders), not just end-to-end fixtures.
- A package's `tests/` folder mirrors the grouping of its `src/` folder; put new tests in the subfolder matching the code under test.
- `corpus_roundtrip` asserts the self-hosted front end parses the whole repo byte-exact with no unexpected diagnostics.
- `semacheck` is a bidirectional gate: every accepted program must produce zero self-hosted sema diagnostics, and every `@expect-error` fixture must be rejected by sema.
  A fixture whose diagnostic belongs to a later phase carries `// @expect-error-at <phase>`; a tag sema outgrows fails the run as stale.
  A fixture the front end rejects never reaches sema at all, so a parse-level error and the semantic errors of the same feature cannot share a file: the parse error is reported alone.
- `codegencheck` runs twice, once per shipped code-generation configuration: `codegencheck` at `-O2` and `codegencheck_unoptimized` at `-O0`.
  `-O0` is what every program built with `-O0` gets, so it is gated exactly as hard as the default is.
- Both arms must keep `selfhost/codegencheck/skiplist.txt` **empty**.
  The list may only shrink: the harness fails if a runnable fixture is neither verified nor listed, and equally if a listed name is no longer a runnable fixture, so the exemption list cannot rot.
- `bootstrap` is the strongest end-to-end gate: `build/host/ens` compiles `selfhost/driver` into stage 2, stage 2's `ens` compiles the same sources into stage 3, and the two stages' object files must hold the same modules with identical bytes.
  Executables are compared as a note only, because linker output carries timestamps.
  The seed built the host compiler but neither stage, so what the seed generates cannot decide this gate: it answers for this tree's code generation alone.
- The `cli_*` jobs (`cli_build`, `cli_runtest`, `cli_overriding`, `cli_dependencies`, `cli_prebuilt`, `cli_toolchain`) all drive `ens`, and each asserts the command's own contract.
  Nothing about another command line is authoritative for them.

## The bootstrap seed

`ens` is written in Ens, so building it needs an `ens` that already runs.
That first one is committed to the repository, one binary per host, and `xmake test` copies the one matching the host into `build/seed/` before anything else:

- `seed/windows-x64/ens.exe`
- `seed/linux-x64/ens`
- `seed/macos-arm64/ens`

A host with no committed seed fails the run by name instead of falling back to another compiler, because a fallback would leave the seed itself untested.
Nothing has to be compiled from C++ before Ens can be, beyond the `ens-lld` linker bridge and the LLVM package every build links against.

**What the seed is for, and what it is not.**
Its one job is to build `build/host/ens` out of the tree, and no test drives the seed itself.
Nothing it produces is compared against anything either, so it cannot pull a gate towards its own behavior: a diagnostic or a code-generation change is measured the run it lands in, not the run after some binary is refreshed.
A seed the sources have outgrown therefore shows up as a failure to build the compiler, before any job starts, and never as drift in what the tests assert.

### Refreshing a seed

A seed goes stale when the sources start using something it cannot compile: a new keyword, a library call it does not have, a rule it does not enforce yet.
The symptom is always the same, and it is loud: `xmake test` stops before any job runs, naming the platform and quoting the build it could not finish.
It never shows up as a test that starts passing or failing differently.

To produce a replacement, run an `ens` that already works on that platform against this tree, and write the result over the committed binary: `ens build selfhost/driver --output seed/windows-x64/ens.exe --stdlib libs`, with the platform folder and file name of the host you are on.
`--stdlib libs` is what makes it this tree's standard library rather than the one belonging to whatever `ens` is doing the building.
That `ens` may be the seed being replaced, a `build/host/ens` from an earlier commit, or an installed toolchain; all that matters is that it compiles these sources.

The new binary lands in the same commit as the change that needed it, because every commit has to be buildable from its own checkout.
A change no seed can compile therefore needs all three refreshed, and each one has to be built on its own platform: seeds are not cross-compiled, and there is no C++ compiler to fall back on any more.
A platform whose seed is too old cannot bootstrap at all until somebody on that platform builds one, so a change that outruns the seeds should not land until every platform has its replacement.

The cost this model accepts is a multi-megabyte binary per platform in git history, growing every time it is refreshed.

## The self-hosted compiler

- `spec.md` and the `tests/` fixtures are what observable behavior answers to, and nothing else is.
  There is no second implementation to compare against and no parity to keep: a weak diagnostic, an unsound accept or a crash is a hole to fix or report, never a shape to copy from anywhere.
  Genuine bugs that block a design choice stop the work; report a minimal repro rather than adopting a workaround design.
- Generated files (`selfhost/frontend/src/syntax/generated/kind.ens`, `nodes.ens`, `factory.ens`, `dump.ens`) say "Generated by syntaxgen - Do not edit".
  Change `selfhost/syntax.grammar` or the emitter in `selfhost/syntaxgen/src/` and regenerate.
- Ens packages use a workspace model: an `ens.package` manifest declares either one package or a workspace of member packages, a package's `dependency` declarations resolve by name against the enclosing workspace's members, and a package is consumed through its `src/` folder.
  Keep knowledge of the manifest format isolated where it already lives, `selfhost/sema/src/program/workspace.ens`; the language server reads manifests through its own copy under `lsp/frontend/module/`.
- Per-file work (parsing, declaration scanning) must stay a pure function with no shared mutable state; cross-file phases are staged and return their results and diagnostics as values.
  Threads are coming to Ens eventually; do not add designs that would need refactoring then.

## Diagnostics

Diagnostics are the user experience of the language and are held to a high bar.

- User-facing messages use the real type/symbol names from the user's program, plain language a beginner can follow, a suggested fix, and where natural a concrete example of the correct form.
- Never copy a weak message for parity with anything; write the good version.
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
A comment only states what the code cannot; never phase tags, decision rationale, or references to another implementation. That context belongs in commits and reviews, not source.

## Process

- Commit messages start with a short imperative line.
- One commit per completed milestone or logical change; unrelated fixes discovered along the way go in their own commits.
- When opening a pull request, prefer small diffs a human can actually review over one huge drop; split large efforts into a sequence of reviewable changes.
- Spec changes are user-facing only and written one sentence per line.
- In reports and reviews, label behavioral claims as verified (you ran it) or inferred (you reasoned from code); do not present inference as established fact.
