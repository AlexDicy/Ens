# Standard library redesign

Design record for the full redesign of `libs/std`, decided section by section starting 2026-08-01.
Backwards compatibility is explicitly not a constraint: any syntax, grammar, behavior, or API may break.
Nothing here is scheduled or in flight; this folder records where the design landed.

The language prerequisites in `01-language-prerequisites.md` must land before the library implementation starts.

| File | Contents |
|---|---|
| `01-language-prerequisites.md` | Language features the redesign requires, and the ones considered and declined |
| `02-conventions.md` | Library-wide policies: errors, naming, iteration, copying, visibility |
| `03-core.md` | `@std.core`: `Error`, `StackFrame`, `Comparable`, `Copyable` |
| `04-collections.md` | `@std.collections`: protocol ladder and the six containers |
| `05-text.md` | `@std.text`: the `string` primitive binding, `StringBuilder`, parsing |
| `06-io.md` | `@std.io`: `Reader`/`Writer`, buffering, memory streams, standard streams |
| `07-fs-process-environment.md` | `@std.fs`, `@std.process`, `@std.environment`: rulings so far, signatures pending |
| `08-testing.md` | `@std.testing` rulings |
| `09-todos.md` | Open questions and deferred items |
| `10-migration-plan.md` | Phase 0/A/B/C/D migration order from today's tree to this design |
