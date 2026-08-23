# @std.testing

`test "..." { }` is a language declaration, so the module holds assertions only.

`assertThrows` takes a closure and the expected kind: `assertThrows(fs.ErrorKind.NotFound, () { try fs.open(missing); })`, spelling pending the closure decision.
Assertion failures print structured diffs: element by element for collections, line by line for multi-line text, pointing at the first difference.
`assertEqual` on floats needs a tolerance form or a ban; undecided.
Each std module keeps the happy-path file and errors file split.
No test may depend on any unspecified order: map iteration, set iteration, or sort tie order.
