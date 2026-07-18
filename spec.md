`TODO`: add visibility to allow only some classes and descendents to access a protected method/member.
`TODO`: field aliases/getters, for example `Vector4` struct can have `x`, `y`, `z`, `w` and `r`, `g`, `b`, `a`.

Primitive types: `bool (1)`, `byte (1)`, `short (2)`, `ushort (2)`, `int (4)`, `uint (4)`, `long (8)`, `ulong (8)`, `float (4)`, `double (8)`. `byte` is unsigned (0..255); `short`/`int`/`long` are signed; `ushort`/`uint`/`ulong` are their unsigned counterparts.

Public by default. Top-level `private` applies to the file scope. When `private` is used in a class or struct it applies to the class/struct level. `Protected` fields/methods in classes and structs apply to the file scope and subclasses in the case of classes. Top-level `protected` is not allowed as it currently carries no meaning.

```ens
private calculateArea(uint width, uint height) -> uint {
    return width * height;
}

printArea() { // no need to specify -> void
    uint width = 20;
    uint height = 18;
    let area = calculateArea(width, height);
    Log.info("Calculated Area: {area}");
    area = calculateArea(height: 56, width: 90);
    Log.info("New area: {area}");
}
```

A function with a non-void return type must return a value on every path through its body: the compiler rejects a function that can reach the end of its body without hitting a `return`, `throw`, `rethrow`, or `panic()`.
An `if`/`else` where every branch exits counts as exiting, as does a `switch` whose arms all exit and a `while (true)` loop with no `break`.

Everything public by default.

Structs automatically implement `copy()` and are `(de)serializable` by default, which also allows printing them as JSON strings. Private and protected fields are included in the serialization.

```ens
struct Rectangle {
    protected uint width;
    uint height;
    string name = "unnamed rectangle ({width}x{height})"; // automatically initialized in order, after width and height have initialized. Defaults are evaluated in declaration order; a default may only reference fields declared earlier.

    getHalfArea() -> double {
        return (width * height) / 2;
    }
}
```

Overloading is allowed, best match arguments first, then visibility.
Two declarations of the same name must differ in parameter count or parameter types.
A call picks the overload whose parameter types match the arguments exactly; when there is no exact match, an overload reachable through implicit widening is chosen.
When two overloads match equally well, the call is a compile error that lists the candidates.
Overloads that are not visible from the call site lose to visible ones; visibility is only an error when no visible overload matches.
Named arguments participate in selection: an overload is only considered when every named argument names one of its parameters.
A subclass may add a new overload of an inherited method name; `override` still requires the exact signature of the method it replaces.
External functions and generic functions cannot be overloaded.

```ens
calculateArea(Rectangle rectangle) -> uint {
    uint area = rectangle.width * rectangle.height;
    Log.info("Calculated Area: {area} for rectangle: {rectangle}");

    // example: calculateArea({width: 20, height: 5});
    // outputs: Calculated Area: 100 for rectangle: {"width": 20, "height": 5, "name": "unnamed rectangle (20x5)"}

    return area;
}
```

```ens
class Animation<S: Shape + Comparable> {
    private S? shape;

    // A constructor is introduced by the `constructor` keyword. It can be a full method or a shorthand which initializes the fields. Methods can have optional parameters, optional parameters must provide a default value. This syntax allows to use either new Animation(); or new Animation(myShape);
    // the default expression must be assignable to the field's declared type.
    constructor(this.shape = null);

    // or:
    // constructor(S? shape = null) {
    //     this.shape = shape;
    // }

    start() -> bool {
        Log.debug("Has shape? {this.shape != null ? "Yes" : "No"}.");

        if (this.shape != null) {
            return true;
        }

        return false;

        // or directly return this.shape != null;
    }

    stop() {
        this.shape = null;
    }
}
```

A class may extend one other class with `extends`. A subclass inherits the base class's fields and methods, and a value of a subclass may be used wherever the base class, or `Base?`, is expected. Arrays are not covariant: a `Derived[]` is not also a `Base[]`.

```ens
class Shape {
    protected int sides;
    constructor(this.sides = 0);
    area() -> int { return 0; }
}

class Square extends Shape {
    int side;
    constructor(int s) {
        super(4);          // run the base constructor first
        this.side = s;
    }
    override area() -> int { return this.side * this.side; }
}
```

Methods are overridable by default. An override must be marked `override` and must match a method declared in a base class; this catches typos and accidental shadowing. Mark a method or a class `final` to forbid overriding or extending it.

`super.method(...)` calls the base class's implementation, bypassing any override. A constructor may call `super(...)` as its first statement to run the base constructor; if it does not, the base class must be constructible with no arguments. `protected` members (see above) are reachable from subclasses.

Class fields may declare default values just like struct fields. Defaults are applied when an instance is created, in declaration order and before the constructor body runs, so constructor assignments overwrite them.

A class may declare a `destructor`, introduced by the `destructor` keyword, to run cleanup when an instance is destroyed.
A destructor takes no parameters, has no return type, and cannot be `throws`; a class declares at most one, and it cannot be called explicitly.
When the last reference to an object is released its destructor runs, followed by each inherited destructor from the most derived class up to the base, and then the object's fields are released.
Destructors are a class-only feature; structs and interfaces cannot declare them.

An `abstract class` cannot be instantiated. It may declare `abstract` methods (a signature with no body), that every concrete subclass must `override`.

A `sealed class` closes its hierarchy: every direct subclass must be declared in the same module as the sealed class, and extending it from another module is a compile error.
A sealed class may be abstract or concrete (`sealed abstract class Expr { ... }`), but it cannot also be `final`, which already forbids subclasses.
Sealing does not constrain the subclasses themselves: they may in turn be sealed, final, abstract, or left open.
Because the compiler sees the whole hierarchy, a `switch` over a sealed class can be checked for exhaustiveness (see the switch section).

---

An `interface` declares a named contract: a set of method signatures with no bodies.
Interfaces are declared at the top level and follow the same `public`/`private` visibility rules as classes; they may be generic, specialized per type-argument set like generic classes.
An interface body contains only method signatures, each ended with `;`.
An interface cannot declare fields, constructors, or method bodies, and it cannot extend or implement anything.
A throwing interface method must list its thrown types explicitly (`load(string path) -> string throws IOError;`), the same rule as abstract methods.

```ens
interface Speaker {
    speak() -> string;
}

interface Source<T> {
    take() -> T;
}
```

A class names the interfaces it implements in an `implements` clause after the optional `extends`, separated by commas: `class Dog extends Animal implements Speaker, Source<string> { ... }`.
The class must provide every method of every listed interface with the exact signature, either declared in the class or inherited from a base class; a missing or mismatched method is a compile error naming the interface and the signature.
A method declared in the implementing class that provides an interface method must be marked `override`, exactly like the override of an abstract base method; a satisfying method inherited from a base class needs no marker.
An abstract class may declare an interface method `abstract override` and leave the body to its concrete subclasses.
A method satisfying a throwing interface method may throw the declared types or their subclasses, never others; satisfying a non-throwing interface method means not throwing at all.
Structs cannot implement interfaces.

An interface name is a reference type usable wherever a class type is: variables, parameters, returns, fields, generic type arguments, `I?`, and arrays under the same element rules as classes.
A value of an implementing class converts implicitly to each interface it implements (and to `I?`); there are no implicit conversions between unrelated interfaces.
A call through an interface-typed value dispatches on the value's runtime type, so a subclass override runs even when the call happens through the interface.
`==` and `!=` on interface-typed values compare identity, exactly like class references.
An interface cannot be instantiated with `new`, and a `weak` field cannot have an interface type; weak references stay class-only.

```ens
Speaker s = new Dog();     // implicit conversion to the interface
print(s.speak());          // runs the implementing class's method
Speaker? quiet = null;     // nullable interface reference
```

---

Classes, structs, functions, and methods may be generic: they declare type parameters in angle brackets and work uniformly over any type argument. A type parameter can be used as a field type, a parameter or return type, a local type, and as the element type of an array.

```ens
class List<T> {
    private T[] items;
    private long count;

    constructor() { this.items = new T[4]; this.count = 0; }

    push(T value) { /* ... grow if full ... */ }
    get(long index) -> T { return this.items[index]; }
    length() -> long { return this.count; }
}

swap<T>(T a, T b) -> T { return b; }
```

A type argument is written in angle brackets wherever the type is used, including at construction:

```ens
let numbers = new List<int>();
numbers.push(5);
let names = new List<string>();
```

A generic type is specialized for each set of type arguments, so a `List<int>` stores its integers directly (no boxing) while a `List<Shape>` stores and reference counts `Shape` objects. Using a generic type without its arguments (just `List`) is an error.

For a generic function, the type arguments can be passed explicitly or, where each one appears directly as a parameter type, inferred from the call:

```ens
swap<int>(1, 2);   // explicit
swap(1, 2);        // T inferred as int
```

A type parameter may declare bounds with `T: Base + Comparable`, joined by `+`: at most one bound may be a class (conventionally written first) and every other bound must be an interface, and listing the same bound twice is an error.
Every type argument must satisfy all bounds, being the class or a subclass of it and implementing each interface; a violation is a compile error naming the failing bound.
The body may use the members of every bound on a value of that parameter.
The `Animation<S: Shape + Comparable>` example above uses exactly this form.

```ens
class Drawer<T: Shape> {
    private T shape;
    constructor(T s) { this.shape = s; }
    area() -> int { return this.shape.area(); }
}

summarize<S: Shape + Comparable>(S value) -> string {
    return value.describe() + " / " + value.compareTo(9);
}
```

A generic class may extend another generic class by naming the base with full type arguments; the arguments may use the subclass's own type parameters. Overrides and virtual dispatch work as with ordinary inheritance, per specialization.

```ens
abstract class Source<T> {
    abstract read() -> T;
}

class Constant<T> extends Source<T> {
    private T value;
    constructor(this.value);
    override read() -> T { return this.value; }
}

Source<int> source = new Constant<int>(5);
source.read();
```

---

Imports are based on paths and qualified by default. Imports are file-local.

```ens
import engine.renderer;
```

This allows usage like `new renderer.Renderer();`

Or:

```ens
import Renderer from engine.renderer;
```

Allows `new Renderer();`

In both cases, the path is `/src/engine/renderer.ens` and the file contains a public `Renderer` class.

Only `public` declarations are accessible from another module. Types (classes and structs) may be brought into scope by name as above, but free functions are always called through their module namespace, never imported by name: write `import engine.renderer;` then call `renderer.configure()`. Importing a function by name (`import configure from engine.renderer;`) is an error.

Two modules may import each other: circular imports are allowed, and declarations resolve across the cycle like any other import.

Importing from packages follows the format `@packageorg.packagename.path`.

```ens
import @std.fs.file; // for external dependency /src/std/fs/file.ens
import Observable from @alexdicy.reactivity.observable; // for class Observable in external dependency /src/alexdicy/reactivity/observable.ens
```

Methods that can throw exceptions are marked with `throws`; any other method can be considered safe. Every thrown value must be an instance of `Error` or a subclass of it. For most methods the set of throwable types is computed by the compiler and shown by IDEs on hover. A method may also declare its thrown types explicitly — `read() -> bytes throws IOError` or `read() -> bytes throws IOError, ParseError`, which is required for abstract methods and forms a contract: an override may throw those types or their subclasses, never others.

If any exception is not handled and the method is not marked as `throws`, this should result in a compilation error explaining which exceptions were not handled and how to handle them (either with a `catch` block or via the `throws` keyword).

These exceptions are always checked. The user can use `panic()` to stop execution, which doesn’t require the use of the `throws` or `throw` keyword.

Methods that throw must be called with `try` as a prefix, even if caught. `Catch` can be added as an additional block of a method.

```ens
class TestRepository {
    getName() -> string? {
        return try queryName();
    } catch (DatabaseError e) { // and other catch blocks if multiple exceptions are thrown
        Log.warn("Database error occurred: {e}");
        return null;
    }

    getNameUnsafe() -> string throws {
        return try queryName();
    } catch (DatabaseError e) {
        Log.warn("Database error occurred: {e}");

        // the current caught exception can be rethrown as-is, rethrow can only be used inside of catch blocks.
        rethrow;
    }

    getNameUnsafeNoCatch() -> string throws {
        return try queryName();
    }

    queryName() -> string throws {
        string? name = Database.runQuery();

        if (name == null) {
            throw new DatabaseError();
        }

        return name;
    }
}
```

Every `Error` carries a **stack trace** captured at the point it is thrown, recording the throwing call and each caller above it. The trace travels with the exception as it propagates, so a handler always sees where the error originated rather than where it was caught. `panic()` captures a trace the same way.

When an exception is never handled, or the program panics, the trace is printed and the program exits:

```
Unhandled exception ParseError: bad token
  at lex (parser.ens:12)
  at parse (parser.ens:15)
  at main (parser.ens:18)
```

A handler can read the trace from a caught error, either preformatted or as structured frames:

```ens
} catch (ParseError e) {
    Log.warn(e.getStackTrace());                // the trace as a string

    StackFrame[] frames = e.stackFrames();   // or as structured frames
    StackFrame origin = frames[0];
    Log.warn("thrown by {origin.function} at {origin.file}:{origin.line}");
}
```

`getStackTrace() -> string` returns the same text shown for an unhandled exception. `getStackFrames() -> StackFrame[]` returns the frames as values, each a `StackFrame` with `function`, `file`, and `line`; `frames[0]` is the throw site.

---

Tests are declared with the `test` keyword, a description string, and a body.
Test declarations live in files ending `_test.ens`, next to the code they cover; everything else in a test file (helpers, classes, imports) is ordinary Ens.
Regular builds skip `_test.ens` files entirely, so tests never ship with the program; `ens test` compiles and runs them.

```ens
import @std.testing;
import Calculator from lib.calculator;

test "addition adds small integers" {
    let calculator = new Calculator();
    try testing.assertEqual(calculator.add(2, 3), 5);
}
```

A test body may call throwing functions with `try` without declaring anything: a test is allowed to throw any `Error`.
A test fails by throwing.
The runner catches the error, reports it, and moves on, so one failure never stops the run.
A `panic()` or a crash still aborts the whole run; tests are not isolated in separate processes yet.

The `@std.testing` module provides `TestFailure`, an `Error` subclass, and assertion helpers that throw it:

- `testing.assertEqual(actual, expected)` and `testing.assertNotEqual(actual, expected)` compare two values of the same type with `==`; the failure message shows both values.
- `testing.assertTrue(condition, message)` and `testing.assertFalse(condition, message)` check a condition; the message is optional, and interpolation at the call site can add context (`"sum was {sum}"`).
- `testing.fail(message)` fails unconditionally.

To assert that some code throws, write a helper whose catch clause swallows the expected type, and fail after the call:

```ens
expectParseFailure(string input) throws {
    let unused = try parse(input);
    try testing.fail("expected a ParseError for '{input}'");
} catch (ParseError expected) {
    return;
}
```

`ens test [--source <folder>] [--tests <folder>] [--filter <substring>]` discovers every `_test.ens` file under the tests folder (the source folder by default, the current one when neither is given) and runs the tests in a deterministic order: files by path, tests in source order.
With `--tests`, test files live outside the source tree: their imports resolve against the source folder first and then the tests folder, and a module present under both folders is an error.
Each test prints one line, a failing test also prints its message and the stack trace of the failure, and the run ends with a summary:

```
PASS addition adds small integers
FAIL subtraction fails on purpose: expected 1, got -1
  at assertEqual (testing.ens:7)
  at "subtraction fails on purpose" (math_test.ens:11)
2/3 tests passed
```

`--filter` runs only the tests whose description contains the substring.
The exit code is `0` when every test passes, `1` when any test fails, and `2` when the tests do not compile.
Test files must have unique file names, and neither a test file nor anything it imports may define `main()`.

---

A type written without a `?` always holds a value and can never be `null`. To allow `null`, suffix the type with `?`.

```ens
class Inner { /* ... */ }
class Outer {
    Inner? inner;     // may be null
    constructor(this.inner = null);
}
```

Any type can be made nullable, including value types: `int?`, `bool?`, an enum, or a struct. A nullable value type carries its own presence, so no value of the underlying type is sacrificed as a marker; `0` and `null` are distinct `int?` values. Comparison with `null`, narrowing, and `??` work the same as for nullable classes.

```ens
findIndex(int[] xs, int wanted) -> long? {
    for (int i = 0; i < xs.length; i = i + 1) {
        if (xs[i] == wanted) { return i; }
    }
    return null;
}

long position = findIndex(numbers, 7) ?? -1;
```

To read through a nullable value, use the safe member operator `?.`. If the value on the left is `null`, the whole expression evaluates to `null` and the right-hand side is not evaluated; otherwise it behaves like `.`.

```ens
Outer? outer = new Outer();
Inner? maybeInner = outer?.inner;   // either null or the field value
int? wheels = car?.wheels;          // value-typed members work too
long size = name?.length ?? 0;
listener?.notify();                 // runs only when listener is present
```

Inside `if (x != null) { ... }` the `x` is considered as the non-nullable form for the rest of the block, so you can use `.` directly. The same narrowing applies to the `else` branch of `if (x == null) { ... } else { ... }`. Reassigning `x` inside the block drops the narrowing from that point on.

A binding also narrows without an explicit check.
Assigning a value whose type is non-nullable to a plain local variable or parameter of nullable declared type narrows it to the non-nullable form from that point on, and a declaration initializer behaves the same way.

```ens
string? s;
s = compute();   // compute() returns a non-null 'string'
s.length;        // s is treated as non-nullable 'string' here

string? t = "x"; // starts narrowed from its initializer
```

This refinement is deliberately limited.
Writing through a member or element path never refines it: `this.field = x` and `arr[0] = x` leave the path nullable, because another reference could write null through the same storage.
Assigning in only one branch of an `if` does not narrow after it: `if (x == null) { x = fallback(); }` leaves `x` nullable below the `if`.

Because narrowing governs only the reads, `== null`, `!= null`, `??`, `?.`, and `?[` on a binding whose declared type is nullable stay legal even where the value has already been proven non-null; the redundant check is simply constant at runtime.
A binding whose declared type is not nullable still rejects these operators.

Narrowing also follows the short-circuit operators and conditions: `x != null && x.ready()` narrows `x` on the right of `&&`, `x == null || x.ready()` narrows on the right of `||`, a conjunction of checks narrows the whole `if` branch or ternary branch, and a loop condition narrows the loop body.

```ens
if (a != null && a.b != null) {
    a.b.use();                    // both links narrowed by the condition
}

while (cursor != null) {
    total = total + cursor.size;  // narrowed by the loop condition
    cursor = cursor.next;
}
```

```ens
draw(Outer? outer) {
    if (outer != null) {
        outer.inner; // outer is treated as non-nullable 'Outer' here
    }

    if (outer == null) {
        return;
    } else {
        outer.inner; // ok in the else branch too
    }
}
```

When one branch of an `if` always exits, the opposite narrowing survives after the `if`, so the `else` above is optional.
A branch exits by `return`, `throw`, or `panic`, and, inside a loop, also by `break` or `continue`.
A loop guard clause therefore narrows the checked value for the rest of the iteration:

```ens
for (let cursor in nodes) {
    if (cursor == null) {
        continue;
    }
    cursor.visit();   // cursor is non-nullable for the rest of the body
}
```

Narrowing extends to **member chains** (`this.field`, `obj.field`, `a.b.c`) and to **array subscripts** (`arr[K]` for an integer-literal index, `arr[i]` for a plain identifier index, arithmetic and call indices are not narrowed). The same `!= null` / `== null` patterns work; the narrowed form holds for as long as nothing invalidates it.

```ens
if (this.shape != null) {
    return this.shape.area;       // this.shape is non-nullable here
}

if (xs[0] != null) {
    xs[0].method();               // literal-index subscript narrows
}

int i = 2;
if (ys[i] != null) {
    ys[i].method();               // identifier-index subscript narrows
}
```

Narrowing is dropped when the analyzer can't prove the narrowed value is still non-null. Specifically:
- Writing to the narrowed path or any deeper part of it (`r.door = null`, `xs[0] = null`).
  Writing through a subscript also drops the narrowing of any element it could alias: `xs[j] = null` drops `xs[0]` because `j` could be `0`, and `xs[0] = null` drops `xs[i]`.
  Elements narrowed at a different literal index are kept: `xs[2] = null` leaves `xs[0]` and `xs[1]` narrowed.
- Reassigning the root variable or, for subscripts, the index variable.
- Any function or method call whose receiver path or class/array-typed argument could touch the narrowed root. Calls that don't touch the relevant root (e.g. `print("hi")`) leave the narrowing intact.
  A call never drops the own narrowing of a plain local variable (or parameter), whether the binding is the call's receiver (`x.method()`) or an argument (`use(x)`): a callee cannot reassign the caller's binding, and the binding's own reference keeps the narrowed object alive.
  What a call does drop is any member-path narrowing (`this.field`, `a.b`) rooted at a value it touches, since the callee may mutate those fields.
  Passing the local as an `out` argument is the sole exception: `out` lets the callee write the caller's variable directly, so it drops the binding's own narrowing too.

A write through a narrowed path is checked against the declared field or element type, not the narrowed one.
Nulling out a just-checked field (`r.door = null`) or storing a base value over an `is`-narrowed one (`c.shape = new Shape()`) is therefore allowed; the write drops the narrowing, and reading the path again requires a new check.

```ens
if (room.door != null) {
    room.door.code;        // ok
    room.door.open();      // call's receiver is rooted at `room`,
    room.door.code;        // error - narrowing dropped
}
```

The `is` type test (described with `as?` further below) narrows by exactly the same rules, including these invalidation points.

---

Numeric values convert between each other with the `as` operator: `expr as Type`. The source and target must both be numeric. Casts between non-numeric types are a compile error.

```ens
long n = 300L;
byte b = n as byte;      // truncating narrow: keeps low 8 bits (44)
int neg = -1;
uint u = neg as uint;    // same-width reinterpret: 0xFFFFFFFF
double d = 3.7;
int t = d as int;        // float -> int truncates toward zero (3)
```

`as` binds tightly to the value just before it. It has higher precedence than `*`, `+`, and unary `-`. To cast a whole expression, parenthesize it:

```ens
int[] arr = new int[4];
long a = arr.length * 2 as long;  // arr.length * (2 as long)
long b = (arr.length * 2) as long; // cast applied to the product
```

---

The `++` and `--` operators add or subtract one from a numeric value in place.
Each works in prefix position (`++x`) and in postfix position (`x++`), and both forms change the operand the same way.
The difference is the value the expression produces: a prefix form evaluates to the new value, and a postfix form evaluates to the value from before the change.
The operand must be an assignable numeric location, such as a variable, a parameter, a field, or an array element, of an integer or floating-point type.
Applying either operator to a literal, a computed expression, a `const` binding, or a non-numeric value is a compile error.

```ens
int i = 0;
int a = i++;      // a is 0, then i becomes 1
int b = ++i;      // i becomes 2, then b is 2
```

---

Class values support runtime type tests with `is` and checked casts with `as?`.
`expr is Type` evaluates to `bool`: true when the value is a non-null instance of `Type` or one of its subclasses.
`expr as? Type` evaluates to `Type?`: the value itself when the test would succeed, and `null` when the value is null or not an instance.

```ens
Shape s = pickShape();
if (s is Circle) {
    s.radius;                          // s is treated as 'Circle' here
}

Circle? c = s as? Circle;              // the circle, or null
int r = (s as? Circle)?.radius ?? 0;
```

The target must be a class or an interface; testing against a struct, primitive, enum, array, or string is a compile error, and so is a nullable target like `as? Circle?`, whose result would already be nullable.
The scrutinee must be a class, an interface, or a nullable form of either, and the target must be related to it: a test that could never succeed (unrelated classes) and a test the static type already satisfies (always true) are both compile errors.
A nullable scrutinee tested against a type it already satisfies is the exception: for `Base? x`, the test `x is Base` is a combined null-plus-type check and is allowed.
An interface target over a class scrutinee is an error only in the impossible case, a `final` class that does not implement it (any other class could have an implementing subclass), or the always-true case where the static class already implements it.
An interface scrutinee may be tested against any class or interface target; the outcome is decided by the value's runtime type.

`if (x is Derived)` narrows `x` to `Derived` inside the branch, following the same rules as null narrowing above: the same paths narrow (locals, member chains, subscripts), `x is Derived && x.derivedMethod()` narrows the right side of `&&`, conjunctions narrow the branch, a loop condition narrows the body, and the same writes and calls drop the narrowing.
Failing the test proves nothing about the value's type, so nothing narrows in the else branch.

`is` sits at the comparison precedence level, so `a is Circle && b is Square` reads as `(a is Circle) && (b is Square)`.
`as?` binds tightly to the value just before it, like `as`.
Inside a generic body the scrutinee may have a type-parameter type; the requirements are then checked against the concrete type of each instantiation.

---

A narrower numeric value automatically converts to a wider one when the conversion preserves every possible value. Narrowing always requires casting with `as`.

```ens
int x = 5;
long y = x;              // int -> long, automatic
int n = arr.length;      // error: long -> int, can be forced with `arr.length as int`
```

Integer literals without a type suffix adapt to the surrounding type when it's an integer that fits the value. With no context they default to `int`. Values out of range produce a specific error.

```ens
byte b = 5;              // OK - 5 fits in byte
long n = 5;              // OK - 5 fits in long
byte big = 300;          // error: 300 does not fit in 'byte' (range -128..127)
```

---

`let` and a typed declaration both introduce a mutable binding. `const` introduces an immutable one: it must be initialized, and assigning to it again, or passing it as `out`, is a compile error. Like `let`, a `const` may infer its type or state it explicitly.

```ens
let count = 0;          // mutable, inferred int
count = count + 1;      // ok

const limit = 10;       // immutable, inferred int
const int max = 100;    // immutable, explicit type
limit = 11;             // error: cannot assign to constant 'limit'
```

`while` repeats its body while the condition holds. A `for` loop comes in two forms. The C-style form has an initializer, a condition, and an update, any of which may be omitted; the initializer is scoped to the loop. The for-each form walks an array element by element, or any iterable object value by value.

```ens
while (i < n) {
    i = i + 1;
}

for (int i = 0; i < xs.length; i = i + 1) {
    total = total + xs[i];
}

for (int x in xs) {     // x takes each element in turn
    total = total + x;
}
```

A class is iterable when it implements the `Iterable<T>` interface from `@std.collections.iterator`, whose single method `makeIterator() -> Iterator<T>` returns an `Iterator<T>` (an interface with `hasNext() -> bool` and `next() -> T`).
The loop calls `makeIterator()` once, then draws values with `next()` while `hasNext()` is true.
A value whose static type is `Iterable<T>` itself can also be iterated.

```ens
import Iterable from @std.collections.iterator;
import Iterator from @std.collections.iterator;

class Range implements Iterable<int> {
    private int low;
    private int high;
    constructor(this.low, this.high);
    override makeIterator() -> Iterator<int> { return new RangeIterator(this.low, this.high); }
}

for (let n in new Range(1, 10)) {
    total = total + n;
}
```

`break` exits the nearest enclosing loop; `continue` skips to its next iteration. Using either outside a loop is a compile error.

The logical operators `&&` and `||` require `bool` operands and short-circuit: the right side is evaluated only when it can change the result, so `a != null && a.ready()` is safe.

The null-coalescing operator `??` takes a nullable value on the left. It evaluates to that value when it is not `null`, and otherwise evaluates and returns the right side. The right side is skipped when the left is non-null.

```ens
Inner? maybe = outer?.inner;
Inner chosen = maybe ?? fallback;   // fallback only when maybe is null
```

An `enum` declares a fixed set of named constant. Enum values compare with `==` and `!=`, and a value prints (and interpolates) as its member name.

```ens
enum Command {
    Initialize,
    Reload,
    Submit,
    Exit,
}

const Command command = Command.Submit;
if (command == Command.Submit) {
    Log.info("running {command}");   // running Submit
}
```

`switch` matches a value against a set of arms and runs (or evaluates to) the first matching arm. It works over an enum, an integer, a string, or a class value. Each arm is written `label -> body`, or `default -> body` for the catch-all; several labels separated by commas share one arm. There is no fall-through, so exactly one arm runs.

A switch over an enum must be exhaustive: it either covers every member or provides a `default`. A non-exhaustive enum switch is a compile error that names the missing members, so adding a member forces every switch over that enum to be updated. A switch over an integer or a string must provide a `default`.

A switch is also an expression: each arm yields a value, the arms unify to a common type (the same way the branches of `?:` do), and the switch evaluates to the matched arm's value. In statement position an arm's body may be a `{ }` block; used as a value, each arm is a single expression.

Unification treats `null` as the absent case of a nullable type: a `null` branch or arm beside a `T` one yields `T?`, and a `T` one beside a `T?` one yields `T?`.
Two class values that share a common ancestor unify to their nearest common base class, so `Circle` beside `Square` yields the closest class both extend.
When either side is nullable, the result is that base class made nullable, so `Circle?` beside `Square` yields `Shape?`.
Classes that share only an interface do not unify, because a class may implement several interfaces at once and the intended one would be ambiguous; give the expression an explicit type in that case.
Unrelated types still do not unify, and branches that are all `null` give the expression no type of its own, so it can appear only where a plain `null` could.

```ens
string? label = hasLabel ? readLabel() : null;   // string beside null -> string?
Shape shape = round ? new Circle() : new Square();  // Circle beside Square -> Shape
int? bonus = switch (rank) {
    1 -> 100,
    2 -> 50,
    default -> null,                             // int beside null -> int?
};
```

When the value is nullable, a `null ->` arm handles the null case and counts toward exhaustiveness alongside the other labels.

```ens
const number = switch (command) {
    Initialize -> 17,
    Reload -> 18,
    Submit, Exit -> 0,            // one arm shared by two members
};

return switch (status) {          // over an int, default required
    200 -> "ok",
    404 -> "missing",
    default -> "other",
};

switch (command) {                // statement form: block or expression arms
    Initialize -> { Log.info("starting"); },
    default -> Log.info("ignored"),
}

let length = switch (name) {      // name is string?, the null case is handled
    "hi" -> 2,
    null -> -1,
    default -> 0,
};
```

A switch over a class or interface value matches on the runtime type instead of on labels.
A type arm is written `is Type binding -> body`; the binding is an arm-scoped constant of type `Type`, and it may be omitted when the value is not needed.
Arm types follow the same rules as the `is` operator: an arm that could never match and an arm the static type already satisfies (which would match everything, `default`'s job) are both compile errors.
An arm may name an interface, and a switch over an interface value may test both classes and interfaces.
Arms are tested in source order, and an arm whose type is already covered by an earlier arm is a compile error, just like a `catch` clause shadowed by a broader one.
Value labels cannot be mixed with type arms; `default` is allowed alongside them, and for a nullable value a `null ->` arm handles the null case exactly as it does elsewhere.

When the value's class is sealed and abstract, the switch may omit `default` by covering every direct subclass; an arm for a subclass also covers all of that subclass's descendants.
A non-exhaustive switch over a sealed hierarchy is a compile error that names the missing subclasses, so adding a subclass forces every such switch to be updated.
A concrete sealed class still needs `default`, because the value may be an instance of the root class itself, which no subclass arm can match.
When the class is not sealed, `default` is required, just like an integer or string switch, and a nullable value additionally needs its `null ->` arm (or `default`).
A switch over an interface value always requires `default`: interfaces are open, so any class anywhere may implement one.
Type arms work in both the statement and the expression forms.

```ens
sealed abstract class Expr { ... }
class Num extends Expr { int value; ... }
class Neg extends Expr { Expr inner; ... }
class Add extends Expr { Expr left; Expr right; ... }

eval(Expr e) -> int {
    return switch (e) {           // exhaustive: every subclass has an arm
        is Num n -> n.value,
        is Neg n -> 0 - eval(n.inner),
        is Add a -> eval(a.left) + eval(a.right),
    };
}

switch (shape) {                  // an open hierarchy needs default
    is Circle c -> { render(c); },
    default -> { },
}
```

---

Arrays are written with `T[]` and are reference types: declaring an array variable binds a pointer to a heap allocation, and copying the variable copies the pointer.

```ens
int[] xs = new int[5];        // 5 ints, zero-initialized
xs[0] = 10;
xs[1] = xs[0] * 2;

let n = xs.length;            // long
```

- `new T[size]` allocates an array of `size` elements. Primitive and reference slots start zero / `null`. Struct slots get the struct's declared field defaults applied to each slot.
- The **innermost** element type must be one whose default value is meaningful, unless the array is filled as it is created (see the fill loop below).
  A non-nullable reference type (class, array, external, string) is otherwise rejected as the element; use the nullable form, `Box?[]` rather than `Box[]`.
  The same rule extends through struct fields: a struct containing a non-nullable reference field cannot be used as an array element.
- `new T[a][b]` allocates a fully-populated multidimensional grid in one call: an outer array of length `a`, each slot holding a freshly-allocated `T[]` of length `b`. The same shape extends to higher dimensions (`new T[a][b][c]`). Because every intermediate level is allocated, types like `int[][]` are valid here even though no intermediate slot is nullable.
- `new T[a][]` allocates only the outer array; inner slots stay `null`. The result type is `T[]?[]`, the deepest unallocated level is reflected in the type by adding a `?`. Trailing empty brackets compose: `new T[a][b][]` produces `T[]?[][]`. Sized brackets must come before any empty ones in a single `new` expression.
- `arr[i]` reads or writes an element. Bounds are checked at every access; an out-of-range index aborts the program.
- `arr.length` returns the number of elements as a `long`.
- `arr.slice(start, end)` returns a new array holding the elements in the half-open range `[start, end)`.
  An invalid range aborts the program.
  Elements are copied the way ordinary assignment copies them, so class elements are shared by reference.
- `T?[]` is an array of nullable `T`, each **element** can be `null`. `T[]?` is a nullable array variable. The variable itself may be `null`. The two compose: `T?[]?` is both. To safely index a nullable array, use `?[i]`: it short-circuits to `null` when the receiver is `null`, otherwise it indexes normally.

```ens
int[]? cache = null;
if (cache != null) {
    cache[0] = 1;             // `cache` is `int[]` here
}
```

```ens
int[][] grid = new int[3][4];     // 3 rows of 4 ints, fully allocated
grid[2][3] = 1;

Box?[]?[] sparseGrid = new Box?[]?[3];   // outer allocated, each row left null
sparseGrid[0] = new Box?[2];             // populate one row by hand
let cell = sparseGrid[0]?[1];            // Box?, null when the row is null
```

An **array literal** `[a, b, c]` allocates and fills an array in one expression. The element type comes from context when one is available (variable type, parameter type, return type); when there isn't a target, the type of the **first element** drives inference for the rest, and remaining elements adapt to it just like ordinary assignments do.

```ens
int[] xs = [10, 20, 30];           // element type from declaration
byte[] buf = ['H', 'i', 0];        // char/int literals narrow to byte
long[] ys = [1, 2L, 3];            // 1 and 3 widen to long
let zs = [1, 2, 3];                // first-wins -> int[]
let g = [[1, 2], [3, 4]];          // nested literal -> int[][]
let h = [[1], []];                 // empty inner adopts first inner's type
```

An empty literal `[]` requires a target type, `let xs = []` is rejected because there is no element to infer from. Write `int[] xs = []` or pass `[]` as an argument where the declared parameter type pins it down.

An array literal is one way to construct an array of a non-nullable reference type: `[new Box(1), new Box(2)]` initializes every slot at construction, and the resulting type is `Box[]`.

```ens
let bs = [new Box(1), new Box(2)];     // bs: Box[]
makeBoxes() -> Box[] {
    return [new Box(1), new Box(2)];
}
```

When the size is not known until run time, `new Box[n]` is legal in exactly one shape: declare a new variable with the allocation, and make the very next statement a loop that fills every slot.
The loop must count from `0` to the array's own `.length`, stepping by one, and its body must be a single assignment to the current slot.
The fill expression must not mention the array being filled.
A zero-length allocation is fine, the loop simply runs zero times.
After the loop the array is fully initialized and behaves like any other array variable.
Anything else, such as a statement between the declaration and the loop, a different loop condition, or a fill expression that reads the array, keeps the allocation an error.

```ens
makeLabels(long n) -> string[] {
    string[] labels = new string[n];
    for (long i = 0; i < labels.length; i = i + 1) {
        labels[i] = "item " + i;
    }
    return labels;
}
```

The element type may itself be an array: `new string[][n]` allocates an outer array whose slots the loop fills with `string[]` values.

---

Strings are immutable text values, written with double quotes (`"hello"`), and are reference types like arrays: a variable binds a reference, and copying it copies the reference. Because strings are immutable, every operation that "changes" a string returns a new one.

- `==` and `!=` compare **contents**, not identity, so `"ab" == "a" + "b"` is true.
- `s.length` returns the number of UTF-8 **bytes** as a `long`.
- `+` concatenates strings. When one side is a string, an integer or `bool` on the other side is converted to text implicitly (the same way `.toString()` would). Types without a string conversion yet are still rejected.
- `.toString()` produces a string from a value explicitly: integer types format as decimal, `bool` as `true` or `false`, and a string returns itself. It can be written directly on a literal, as in `42.toString()`.
- `s.toBytes()` returns the UTF-8 bytes as a `byte[]`, and `string.fromBytes(bytes)` builds a string from a `byte[]` by interpreting it as UTF-8.
- `s.contains(needle)` reports whether `needle` occurs in `s`.
  `s.indexOf(needle)` returns the byte offset of the first occurrence as a `long`, or `-1` when absent; an empty needle is found at offset `0`.
- `s.substring(start, end)` returns the bytes in the half-open range `[start, end)` as a new string.
  Offsets are byte offsets, like `length` and `indexOf`.
  An invalid range (a negative start, a start past the end, or an end past the length) aborts the program.

```ens
let greeting = "Hello, " + name + "!";
let n = greeting.length;            // long, the byte count
if (name == "world") { /* ... */ }
let label = count.toString();       // "0", "42", "-7"
let raw = greeting.toBytes();       // byte[]
let back = string.fromBytes(raw);   // string
let at = greeting.indexOf("llo");   // 2
let has = greeting.contains("!");   // true
let word = greeting.substring(0, 5); // "Hello"
```

**Interpolation** embeds expressions in a string with `{ }`. Each hole is converted to text the way `.toString()` would, then the literal parts and holes are joined into one new string. Write `\{` and `\}` for literal braces.

```ens
let report = "Area: {width * height} for {width}x{height}";   // "Area: 100 for 20x5"
let status = "done={finished}, items={count}";                // bool and integer holes
let braces = "use \{these\} verbatim";                        // "use {these} verbatim"
```

Holes currently accept string, integer, `bool`, and enum values; convert other types explicitly with `.toString()` first.
Inside a generic body a hole may hold a value of a type-parameter type; the requirement is then checked against the concrete type of each instantiation.

---

Native libraries can be called from Ens through `external` declarations. They are always written at the top of a source file, alongside `import`s and type declarations.

```ens
external type HANDLE;

external from "kernel32" {
    ReadFile(HANDLE h, byte[] buf, uint n, out uint bytesRead, HANDLE? ov) -> int;
    CloseHandle(HANDLE h) -> int;
}

read(HANDLE h, byte[] buf) -> uint {
    uint bytesRead = 0;
    let ok = ReadFile(h, buf, buf.length as uint, out bytesRead, null);  // buf.length is `long`
    if (ok == 0) {
        panic("read failed");
    }
    return bytesRead;
}
```

- `external type Name;` declares an opaque foreign handle. The handle is passed around and compared with `null`, but it has no members.
- `external from "libname" { ... }` groups foreign function signatures. The library name is what the linker will look for (e.g. `"kernel32"` resolves to `kernel32.lib` on Windows; `"c"` is libc on Unix and is auto-linked).
- The `out` modifier marks a parameter the C function writes back to. At the call site, the caller passes an initialized local variable as `out name`. The variable's type must match the declared parameter type exactly.
- A `string` argument is converted automatically to a NUL-terminated UTF-8 buffer at the call boundary. The C function must not retain that pointer past the call.

Memory is managed automatically through Automatic Reference Counting (ARC).

- **Classes** are heap-allocated reference types. Each instance carries a refcount; when the last reference goes out of scope, the instance is freed.
- **Structs** are value types. They are copied on assignment and passed by value.

Atomics happen only at allocation, at assignment of class-typed fields, and at scope exit.

```ens
class Texture { /* ... */ }

drawSprite(Texture tex) {
    // no retain at entry, no release at exit. Caller's reference owns +1
    tex.bind();
}

renderFrame() {
    let t = new Texture();   // +1
    drawSprite(t);           // zero atomics at the call boundary
    drawSprite(t);           // zero atomics
}                            // t released here
```

Reference cycles between class instances are not collected automatically and leak. A class that needs to hold a reference back to its owner should use the `weak` annotation.

```ens
class Parent {
    Child? child;
}

class Child {
    weak Parent? parent;
}
```

`weak` fields must be nullable class types. They don't contribute to the strong refcount, so they don't keep objects alive. When the referenced object dies, every weak reference to it reads as `null`.

The compiler performs **escape analysis** to elide retain/release pairs and large-struct copies when it can prove a value does not escape its scope. The `--explain-arc` flag surfaces what was elided for diagnostics.

---

The standard library is an external package, imported with `@`, and is opt-in: its declarations are visible only after they are imported. The `std.system` module wraps common operating system facilities and reports failures as exceptions. Import the module to call its functions through the `system` namespace, and import any types you use by name:

```ens
import @std.system;
import File from @std.system;

writeGreeting() -> int throws {
    File file = try system.openFile("greeting.txt", "w");
    byte[] bytes = new byte[2];
    bytes[0] = 'h';
    bytes[1] = 'i';
    let written = try file.write(bytes);
    return file.close();
}
```

`system.exists(path)` reports whether a readable file exists at the path, without throwing: a missing path or a directory reports `false`.

---

Every value has a `hash()` method returning a `long`. Value types (primitives, enums, strings, structs) hash by their contents, so equal values hash equally; classes and arrays hash by identity, matching how `==` compares them. A class can declare its own `hash() -> long` to control its hashing (a method named `hash` must have exactly that signature), paired with `equals(C other) -> bool` - a method taking a single parameter of the class's own type `C` - to control equality. When a class declares such an `equals`, `==` and `!=` on that class compare by content - an identity and null check first, then `equals` - rather than by reference identity; the method must return `bool` and cannot be `throws`. Both `hash` and `equals` are written with `override`, since they replace a class's built-in identity hash and equality. The two are a matched pair: a class that declares one must declare the other, so equal instances always hash equally. The `Hashable` interface from `@std.hash` names the hashing contract for generic bounds, and every type satisfies it.

The collection modules build on hashing and iteration:

- `List<T>` from `@std.collections.list` is a growable array: `push(value)`, `pop()` removing and returning the last value, `get(index)`, `set(index, value)`, and `length()`, plus `toArray()` returning a fresh right-sized `T[]` holding the current contents. Iterating a list yields its values in insertion order.
- `Map<K, V>` from `@std.collections.map` maps keys to values: `set(key, value)` inserts or overwrites, `get(key)` returns `V?` (`null` when absent), plus `contains(key)`, `remove(key)`, `length()`, and `keys()` / `values()` snapshots. Iterating a map yields `Pair<K, V>` entries (from `@std.collections.pair`) with `key` and `value` fields.
- `Set<T>` from `@std.collections.set` stores each value once: `add(value)` returns whether the value was new, plus `contains(value)`, `remove(value)`, `length()`, and `items()`. Iterating a set yields its values.

```ens
import Map from @std.collections.map;
import Set from @std.collections.set;

let ages = new Map<string, int>();
ages.set("ada", 36);
int age = ages.get("ada") ?? 0;

let seen = new Set<string>();
seen.add("ada");

for (let entry in ages) {
    print(entry.key + " is " + entry.value);
}
```

Keys are matched with `==` and bucketed with `hash()`: strings by contents, value types by value, and classes by identity - unless the key class declares `equals` (with its paired `hash`), in which case its keys match by content. Struct keys are not supported yet because structs cannot be compared with `==`.

`StringBuilder` from `@std.text.stringbuilder` accumulates text in a growable buffer, so building a string piece by piece stays linear where repeated `+` on immutable strings would re-copy the whole prefix.
`append(value)` accepts a string, an integer, or a `bool`; `length()` returns the number of bytes written so far; `toString()` returns the accumulated text and leaves the builder usable.
`appendByte(value)` appends one raw byte to the buffer, where `append` on the same value would format it as decimal text.

```ens
import StringBuilder from @std.text.stringbuilder;

let report = new StringBuilder();
report.append("processed ");
report.append(count);
report.append(", ok: ");
report.append(allPassed);
print(report.toString());
```
