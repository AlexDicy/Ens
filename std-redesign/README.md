# Standard library redesign

Design record for the full redesign of `libs/std`, decided section by section starting 2026-08-01.
Backwards compatibility is explicitly not a constraint: any syntax, grammar, behavior, or API may break.

The design is implemented as of 2026-09-19: every phase of `10-migration-plan.md` has landed through D3, and what remains is D4, the release whose seed makes the new library the one every consumer builds against.
This folder is the record of what was decided and why, not a plan to work from.
The chapters' signature blocks were read against `libs/std` on that date and corrected where they had fallen behind it.
Any later difference between a chapter and the code is recorded in `09-todos.md`, and the code is the truth.

| File | Contents |
|---|---|
| `01-language-prerequisites.md` | Language features the redesign required, and the ones considered and declined |
| `02-conventions.md` | Library-wide policies: failures, errors, naming, iteration, copying, resources, layering |
| `03-core.md` | `@std.core`: `Error`, `StackFrame`, `Comparable`, `Copyable` |
| `04-collections.md` | `@std.collections`: protocol ladder and the six containers |
| `05-text.md` | `@std.text`: the `string` primitive binding, `StringBuilder`, the numeric bindings, parsing |
| `06-io.md` | `@std.io`: `Reader`/`Writer`, buffering, memory streams, standard streams |
| `07-fs-process-environment.md` | `@std.fs`, `@std.process`, `@std.environment`, `@std.thread`: `Path` and `File`, the temporary guards, `run`/`spawn`, the environment snapshot, the wait |
| `08-testing.md` | `@std.testing`: `TestFailure`, the assertions, and the rulings behind them |
| `09-todos.md` | Open questions and deferred items |
| `10-migration-plan.md` | Phase 0/A/B/C/D migration order, and what each milestone landed |
