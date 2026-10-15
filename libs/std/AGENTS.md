# The standard library

The conventions the API of `libs/std` follows, on top of the code style in the root `AGENTS.md`.

- Each area of the library has one error class, declared once, with an `ErrorKind` enum beside it when the class covers more than one condition, as `@std.fs` has `FileSystemError` in `@std.fs.error`.
- A stream operation raises `IoError` from `@std.io.streams`, whichever area it is in.
- No lazy adapter chains, ever: no `map`, `filter`, `take` or `fold` pipelines; iteration is a `for` loop, and the library ships terminal helpers that return concrete values.
- A method takes a closure only at a leaf, as an order, a test or a factory such as `sort(order)`, `removeWhere(test)` or `getOrInsert(key, make)`, and only after weighing it against the loop a caller would write instead.
- A mutable type pairs its verbs: the imperative changes the value in place and the past participle returns a new one, as `List.sort()` and `List.sorted()` do.
- An immutable type such as `string` or `Path` has only the form that returns a new value, named as the imperative, as `trim()` and `normalize()` are.
- `toX()` returns a copy, `asX()` returns a cheap view sharing storage, and text becomes a value through the `parseX(text)` functions of `@std.text.parse`.
- A size is `length()`, beside `isEmpty()`, and never `size()`.
- A `bool` member is `is` with an adjective, as in `isEmpty()`, or a plain verb, as in `contains()` or `exists()`, and never a bare adjective.
- Arguments go data first and options last, and a source before its destination.
- No test depends on an order the library leaves unspecified, such as the walk of a `Map` or a `Set` or the order a sort leaves values it calls equal.
- A doc comment states the rule, then its edge cases, in prose, the way the methods of `Path` in `@std.fs` do.
