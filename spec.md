`TODO`: add visibility to allow only some classes and descendents to access a protected method/member.
`TODO`: field aliases/getters, for example `Vector4` struct can have `x`, `y`, `z`, `w` and `r`, `g`, `b`, `a`.

Primitive types: `bool (1)`, `byte (1)`, `short (2)`, `ushort (2)`, `int (4)`, `uint (4)`, `long (8)`, `ulong (8)`, `float (4)`, `double (8)`, `char (4)`. `byte` is unsigned (0..255); `short`/`int`/`long` are signed; `ushort`/`uint`/`ulong` are their unsigned counterparts.
`char` is an unsigned 32-bit Unicode scalar value (0..0x10FFFF); it counts as an integer type, and converts to text as the character it denotes.

Visibility has three tiers, `private` < `public` < `export`, with `protected` alongside them.
Everything is private by default.
An unmarked top-level declaration is visible only within its file, and an unmarked class or struct member, including a constructor, is visible only within its type.
`public` makes a declaration visible to every module in the same package; other packages cannot see it.
`export` makes a declaration visible to the packages that consume this one through `@` imports, including programs using `@std`.
`protected` keeps its own meaning for class and struct members: the declaring file, plus subclasses in the case of classes.
Top-level `protected` is not allowed.
Struct fields follow their struct's visibility; a field that writes its own modifier, such as `private`, opts out.
Class members never follow their class: they stay private unless marked.
The one exception is a method that replaces a behavior the language already provides for its type: a struct's `toString`, `hash`, and `equals`, and a class's `hash` and `equals`.
The language calls such a method wherever the type is used, so it follows its type's visibility when unmarked and may not be marked less visible than its type.
Interface members carry no visibility of their own: they always follow the interface, and writing a visibility modifier on an interface member is an error.
Enum cases follow their enum.
A member may not be declared more visible than the type that contains it: an `export` method on a `public` class is an error, never a silent cap.
Writing the default explicitly, such as `private` on a class member, is allowed.
A declaration's signature may not mention a type less visible than the declaration itself; this covers parameter types, the return type, declared thrown types, field types, a base class, implemented interfaces, and generic arguments and bounds.
A protected member is held to the same rule at the widest scope its class can be subclassed from: the file for a `final`, `sealed`, or file-private class, the package for an open `public` class, and everywhere for an open `export` class, whose external subclassers must be able to name every type its protected members mention.
A `test` declaration sees its file's private top-level declarations like any other code in the file, but not the private members of types.

```ens
public calculateArea(uint width, uint height) -> uint {
    return width * height;
}

printArea() { // private by default; no need to specify -> void
    uint width = 20;
    uint height = 18;
    let area = calculateArea(width, height);
    Log.info("Calculated Area: {area}");
    area = calculateArea(height: 56, width: 90);
    Log.info("New area: {area}");
}
```

A function with a non-void return type must return a value on every path through its body: the compiler rejects a function that can reach the end of its body without hitting a `return`, `throw`, `rethrow`, or a call that never returns (`panic()` or any `noreturn` function).
An `if`/`else` where every branch exits counts as exiting, as does a `switch` whose arms all exit and a `while (true)` loop with no `break`.

A function or method may be declared `noreturn`, a modifier stating that it never returns to its caller: every path through its body ends by throwing, by calling `panic`, or by calling another `noreturn` function, or the body loops forever.
A `noreturn` declaration has no return type, since it returns nothing at all, so writing a `-> T` clause alongside it is an error; it combines with visibility, `override`, `final`, and `throws`, but is not allowed on a constructor, a destructor, or a test.
The compiler holds the body to this promise: a `return` statement is rejected, and a body that can reach its end is rejected, reusing the same path analysis as the missing-return check.
A call to a `noreturn` function ends the path it sits on, exactly as `panic()` does, so a value-returning function may close a branch with such a call and still be accepted, and a value narrowed before the call stays narrowed afterward.
The built-in `panic` is itself a `noreturn` function, so this is one rule rather than a special case.
An abstract or interface method may be `noreturn` as part of its contract, and an override of a `noreturn` method must remain `noreturn`, because callers rely on it never returning.
A throwing `noreturn` function still takes part in checked exceptions as usual: its call sites need `try`, and its declared or inferred `throws` set applies normally.

```ens
public noreturn fail(string message) throws {
    throw new TestFailure(message);
}
```

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

A struct value is built with a context-typed aggregate literal or a constructor call.

An aggregate literal `{field: value, field: value}` names each field it sets, and takes its type from the surrounding context: the declared type of a variable, a parameter, a return type, an assignment target, or an array element.
Every field that has no declared default must be listed; a field that has a default may be omitted to accept that default.
Naming a field the struct does not have, listing the same field twice, leaving out a required field, or giving a value that is not assignable to the field's type are each errors.
A field's visibility is respected: a private field can only be set from inside the struct that declares it, the same rule as a direct field assignment.
A literal with no context to infer its type from, such as `let p = {x: 1};`, is an error; annotate the target or use a constructor.

A struct may declare a `constructor` with the same keyword and shorthand as a class, and it is invoked by writing the struct's name followed by arguments, for example `Point(1, 2)`.
Construction is by value and never uses `new`, which stays reserved for classes and arrays.
A generic struct is constructed the same way, with the type arguments written on the name: `Pair<int, string>(1, "x")` resolves the constructor on that instantiation, just as `Pair<int, string> value = {key: 1, value: "x"}` builds the instantiation from a literal.
The two forms coexist: a struct that has a constructor can still be built from a literal.

A struct local must be built as a whole, with a literal or a constructor, before any field of it is read or written.
`Point p; p.x = 1;` is an error because `p` is used before it holds a value; write `Point p = {x: 1, y: 2};` and then mutate a field of the already-built value.

A struct field may have any type, including a non-nullable one such as a class, a string, an array, or another struct that has no default.
A struct with such a field has no default value of its own, so it cannot be an array element or a field left without a default, though it can still be built with a literal or a constructor wherever a value is needed.

Two values of the same struct type compare with `==` and `!=` field by field, in declaration order, stopping at the first field that differs.
Each field compares by its own `==`: primitives and enums by value (so IEEE rules hold, and a `float` or `double` field that is `NaN` never equals itself), strings by content, class and array fields by reference identity unless the two objects' run-time class opted into content equality, a nested struct memberwise unless it declared its own equality, and a nullable field null-aware (both `null` are equal, one `null` is unequal, otherwise the inner values compare).
Comparing two different struct types is an error, and so is comparing structs whose type has a field with no `==` of its own, such as an `external` handle; the error names the offending field.
A struct customizes equality by declaring `equals(S other) -> bool` - a method taking a single parameter of the struct's own type `S` - which then decides `==` and `!=` for that struct everywhere it is compared, including as a field of another struct, as an array element and as a collection key.
Such an `equals` replaces the memberwise comparison the language provides, so it is written `override`, and it must be paired with an `override hash() -> long`: a struct that declares one must declare the other, exactly as a class must, so equal values always hash equally.
A method named `equals` whose single parameter is some other type is an ordinary method, and `==` on that struct stays memberwise.

A struct serializes to a JSON string through `.toString()` and in interpolation holes, honoring the default-serialization promise.
The form is a JSON object listing every field, including private and protected ones, in declaration order: `{"field": value, ...}`.
Numbers render as decimals, `bool` as `true` or `false`, strings and enum members as JSON-quoted text (with `"`, `\`, and control characters escaped), a `char` as a one-character quoted string with its scalar encoded to UTF-8 and escaped the same way, an absent nullable field as `null`, and a nested struct as its own JSON object.
A struct that declares its own `toString` method uses that method instead; because it replaces the built-in form, it is written `override toString() -> string`, taking no arguments and never `throws` - an interpolation hole has nowhere to write a `try`.
A struct is serializable only when every field is: a value type, a string, an enum, one of those made nullable, or a nested such struct.
A field that is a class, an array, or an external handle has no JSON form and makes serializing the struct an error that names the offending field, mirroring the `==` rule.

Overloading is allowed, best match arguments first, then visibility.
Two declarations of the same name must differ in parameter count or parameter types.
A call picks the overload whose parameter types match the arguments exactly; when there is no exact match, an overload reachable through implicit widening is chosen.
An argument that names no type of its own, such as an empty array literal or a struct literal, takes no part in choosing the overload; once an overload is chosen the argument is typed from the parameter it maps to, and an argument that parameter cannot type is an error.
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
`override` on a method that overrides nothing is an error, so the marker always names something real: a base or interface method, or a behavior the language provides for the type, which for a struct means only `toString`, `hash`, and `equals`.
The marker is required for all three, so a struct that declares a `toString`, a `hash`, or an `equals` always writes it as an `override`.

`super.method(...)` calls the base class's implementation, bypassing any override. A constructor may call `super(...)` as its first statement to run the base constructor; if it does not, the base class must be constructible with no arguments. `protected` members (see above) are reachable from subclasses.

Class fields may declare default values just like struct fields. Defaults are applied when an instance is created, in declaration order and before the constructor body runs, so constructor assignments overwrite them.

A field whose type has no default value, such as a class, a string, an array, or a struct without one, and that is not made nullable, must be definitely assigned on every path through each constructor.
A `this.field` parameter, a declared field default, and a `this.field = ...` assignment in the body all count, and assigning the field in every branch of an `if` or `switch` satisfies the rule exactly as a single unconditional assignment does.
A constructor that leaves such a field unassigned on some path, for example by returning early before it is set, is a compile error, and a class that has such a field but declares no constructor at all is rejected the same way.

A class may declare a `destructor`, introduced by the `destructor` keyword, to run cleanup when an instance is destroyed.
A destructor takes no parameters, has no return type, and cannot be `throws`; a class declares at most one, and it cannot be called explicitly.
When the last reference to an object is released its destructor runs, followed by each inherited destructor from the most derived class up to the base, and then the object's fields are released.
Destructors are a class-only feature; structs and interfaces cannot declare them.

An `abstract class` cannot be instantiated. It may declare `abstract` methods (a signature with no body), that every concrete subclass must `override`.
`abstract` needs a type that can have subclasses, so it is not allowed on a struct member: a struct is never a base type, and nothing could ever implement the member.

A `sealed class` closes its hierarchy: every direct subclass must be declared in the same module as the sealed class, and extending it from another module is a compile error.
A sealed class may be abstract or concrete (`sealed abstract class Expr { ... }`), but it cannot also be `final`, which already forbids subclasses.
Sealing does not constrain the subclasses themselves: they may in turn be sealed, final, abstract, or left open.
Because the compiler sees the whole hierarchy, a `switch` over a sealed class can be checked for exhaustiveness (see the switch section).

---

An `interface` declares a named contract: a set of method signatures with no bodies.
Interfaces are declared at the top level and follow the same visibility tiers as classes; they may be generic, specialized per type-argument set like generic classes.
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

Source files are UTF-8 text.
A byte order mark at the very start of a file is accepted and skipped; the same bytes anywhere else in the file are an error.

A module's private declarations never leave its file; `public` declarations are visible to every module in the same package, and `export` declarations also to the packages that consume it.
Types (classes and structs) may be brought into scope by name as above, but free functions are always called through their module namespace, never imported by name: write `import engine.renderer;` then call `renderer.configure()`.
Importing a function by name (`import configure from engine.renderer;`) is an error.

Two modules may import each other: circular imports are allowed, and declarations resolve across the cycle like any other import.

A short list of standard-library modules is imported implicitly, today just `@std.core`: its exported names `Error` and `StackFrame` are in scope in every module with no import written.
A declaration of your own with one of those names takes precedence inside the module that declares it.
Every other module of the standard library has to be imported.

A program's entry point is the top-level function `main` in its **main module**: `src/main.ens` for a package or a folder of sources, or the compiled file itself for a single-file program.
No other module may define a top-level `main`; the compiler rejects one wherever it is loaded from, including through an import of another package's main module.

`main` is declared either as `main()` or as `main() -> int`, and it may add `throws` or `noreturn` like any other function.
The `int` it returns becomes the process exit code, a `main()` that returns nothing exits with 0, and an exception that escapes `main` is reported on stderr and exits with 1.
Any other return type is an error, and so are parameters and type parameters: nothing passes arguments to `main`, and a program reads its command line through `system.arguments()` from `@std.system`.

Importing from packages follows the format `@packageorg.packagename.path`.

```ens
import @std.fs.file; // for external dependency /src/std/fs/file.ens
import Observable from @alexdicy.reactivity.observable; // for class Observable in external dependency /src/alexdicy/reactivity/observable.ens
```

A package boundary is crossed exactly when a module is consumed through an `@` import from another package, including `@std`.
Only `export` declarations are visible across that boundary; `public` stops at the edge of the declaring package.
A program and its tests form one package, so tests see the `public` declarations of the sources they test.
Protected members of an exported non-final class are visible to subclasses in consuming packages too; no `export protected` spelling exists or is needed.

---

Every package is described by an `ens.package` manifest in its root folder.
The manifest uses declaration notation: each declaration is introduced by a keyword, package names are dotted paths, physical values such as versions and folders are string literals, and a blockless declaration ends with `;`.
Comments work exactly as in source files.
A manifest holds exactly one declaration, either a package or a workspace.

```ens
package alex.jsonkit {
    version "1.3.0";
    ens "1.2";

    dependency ens.frontend;
    dependency alex.json "2.0";

    native zlib;
    native libc system;
    native llvm {
        windows "LLVM-C";
        linux "LLVM-18";
        macos "LLVM";
    }
}
```

A package's sources live in its `src/` folder, with its tests in a sibling `tests/` folder.
A package whose `src/main.ens` defines `main()` is an application and builds to an executable; a package without one is a library.
Every package declares `ens`, the language version it is written for as major.minor; `version`, the package's own version in dotted numerals, is optional.
Both are informational today: nothing gates on their values yet.
Each `dependency` declares a package this one may import with `@`: the leading segments of an `@` import select the dependency with the longest matching name, and the remaining segments name the module inside it.

A workspace groups packages that are developed together.
Its manifest lists the member folders, each relative to the manifest and written with forward slashes:

```ens
workspace {
    member "syntaxgen";
    member "frontend";
    member "sema";
}
```

Every member folder must itself contain a package manifest.
Dependencies resolve by name against the workspace: `dependency ens.frontend;` in one member finds the member whose manifest declares `package ens.frontend`, wherever that folder sits, so package names must be unique within a workspace.
A dependency that resolves to a workspace member never carries a version (and never a `from` clause); members are used exactly as checked out.
Every other dependency declares the version this package requires, and a dependency that is neither a member nor overridden names its git source with `from`:

```ens
dependency alex.json "2.0" from "https://github.com/alex/json.git";
```

`@std` is built in and is never declared as a dependency.

The compiler finds the governing manifest by walking up from the compiled sources to the nearest folder containing `ens.package`, the way `git` finds a repository from a subfolder.
A file with no manifest anywhere above it compiles standalone, with only `@std` available.
Sources that sit under a workspace root but outside any member package likewise see only `@std`: a workspace declares its members without being a package itself.

Overrides redirect a dependency to a local folder, for example to build against a fix in a package checked out elsewhere.
They live in an `ens.overrides` file next to the manifest, and that file is meant to stay out of version control: overrides describe one machine's checkout, not the project.

```ens
overrides {
    override alex.library "../library";
}
```

Only the overrides next to the root manifest of a build apply, and they apply build-wide: every dependency on the overridden name, in every package of the build, resolves to the given folder.
The target folder's manifest must declare exactly the overridden package name, the dependency keeps its declared version (the override redirects where the source comes from, not what the package requires), and every build that uses an override prints a notice.

A git-sourced dependency's version selects a tag in the named repository: the tag spelled exactly like the version, or the same spelling with a `v` prefix (`2.0` or `v2.0`); when both exist the version is ambiguous and the build fails.
Only tags are fetched, never branches or commits: a tag names a release, and unreleased work is brought in by pointing an override at a local checkout.
The repository's manifest at that tag must declare the required package, or be a workspace with a member that declares it.
A fetched package must be self-contained: a package that uses git submodules is rejected, so either declare that code as a dependency too or commit the files into the repository.
A fetched package's own git-sourced dependencies are fetched the same way, and its requirements join the build's.
When several packages require the same package, every requirement must name the same URL, and the build uses the highest required version; requirements that span different major versions are an error naming the requirers.
Versions compare numerically, component by component.
Fetched packages land in a content-addressed cache at `~/.ens/cache` (the `ENS_CACHE` environment variable overrides the location), shared by every build on the machine.

The first build that resolves git-sourced packages or binds a prebuilt artifact writes `ens.lock` next to the root manifest.
The lock records each fetched package's exact version, source URL and commit, a hash of the fetched content, and its own requirements, so a later build reproduces the same result without touching the network, and a moved tag or altered content is detected and rejected.
Commit `ens.lock` to version control; it is machine-owned and never edited by hand.
Workspace members and overridden packages are never pinned in it: a member is used exactly as it is checked out and an override only says where a package comes from, so neither has a version, a source, or a commit to record.
A workspace member that binds a prebuilt artifact is still listed under its own name, so that artifact and its hash are recorded alongside the fetched packages' rather than nowhere.
Builds keep the lock current: when the manifests' requirements change, the build re-resolves what changed, rewrites the lock, and prints a summary of the difference.
`--locked`, accepted by build, check, run, and test, turns any needed lock change into an error, which is what a CI build wants.
`--offline` forbids all network use: anything not already in the local cache fails the build by name.

`native` declarations name the native libraries the package's `external` blocks bind to (see the section on native calls), and tell the linker what to link.
`native libc system;` declares a library the platform links by default, so nothing extra is passed to the linker.
`native zlib;` links the library under its conventional platform name.
The block form spells out base names per platform (`windows`, `linux`, `macos`), several per platform when needed.
A platform may instead bind a prebuilt artifact: `windows artifact "https://example.com/z.lib" hash "sha256:...";` downloads the file once into the cache, verifies it against the declared sha256 hash (a mismatch fails the build), and passes it to the linker.
An artifact is a single library file for its platform.
Artifact bindings and their hashes are recorded in `ens.lock`, so the exact native code a build links is reviewable in one place.
When two packages in one build declare the same native library, the declarations must be identical; identical declarations are linked once.

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
    Log.warn(e.stackTrace());                // the trace as a string

    StackFrame[] frames = e.stackFrames();   // or as structured frames
    StackFrame origin = frames[0];
    Log.warn("thrown by {origin.function} at {origin.file}:{origin.line}");
}
```

`stackTrace() -> string` returns the same text shown for an unhandled exception. `stackFrames() -> StackFrame[]` returns the frames as values, each a `StackFrame` with `function`, `file`, and `line`; `frames[0]` is the throw site.

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

`ens test [path] [--tests <folder>] [--filter <substring>]` discovers every `_test.ens` file under the tests folder (a package's `tests/` folder, or the source folder itself when there is none) and runs the tests in a deterministic order: files by path, tests in source order.
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
Assigning in only one branch of an `if` refines the value below only when every other branch proves it non-null too (see the join rule below): `if (ready) { x = fallback(); }` leaves `x` nullable below, because the path that skips the assignment learns nothing from `ready`.

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

A narrowing holds after an `if`/`else` (or a `switch`) when it holds at the end of every branch that can fall through: the branches are intersected at the merge.
A branch that always exits - by `return`, `throw`, or `panic`, and, inside a loop, also by `break` or `continue` - reaches nothing below the merge, so it places no constraint on the result.
This makes the `else` above optional, and it lets a value narrow when the branches prove the fact in different ways: after `if (x == null) { x = fallback(); }` the value is non-null below, because the then-branch reassigned it to a non-null value while the else-path failed the `== null` check, so both paths reaching the merge prove it.
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
  A constructor call `new T(...)` reaches its class/array arguments the same way an ordinary call does, so it drops the member-path narrowings rooted at them too.
  A call never drops the own narrowing of a plain local variable (or parameter), whether the binding is the call's receiver (`x.method()`) or an argument (`use(x)`): a callee cannot reassign the caller's binding, and the binding's own reference keeps the narrowed object alive.
  What a call does drop is any member-path narrowing (`this.field`, `a.b`) rooted at a value it touches, since the callee may mutate those fields; touching a member chain such as `r.door` therefore drops the paths under `r` but keeps `r`'s own narrowing.
  Passing the local as an `out` argument is the sole exception: `out` lets the callee write the caller's variable directly, so it drops the binding's own narrowing too.

Inside a loop a narrowing must hold on every iteration, so a narrowing established before the loop is dropped at the loop's entry when any statement in the body (or a `for` update) could write its path: a later write would otherwise leave an earlier read in the body using a value that is already stale on the next pass.
A narrowing the loop condition or an in-body guard clause re-establishes on each iteration is unaffected, so `while (x != null)` loops and guard-narrowed loops keep working.

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

A numeric enum converts to an integer type with `as`, yielding the member's assigned value: `Errno.EACCES as int` is `13`, and `as long` is the widest form; a narrower integer target truncates by the same rules as above.
This direction never fails, so it is always `as`, never `as?`.
A plain enum has no numeric value and cannot be converted in either direction; the error explains how to give its members values.

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

An integer converts to a numeric enum with `as?`, which evaluates to the enum made nullable: the member whose assigned value equals the integer, or `null` when no member has that value.

```ens
Errno? e = code as? Errno;             // the matching member, or null
Errno chosen = 13 as? Errno ?? Errno.EPERM;
```

The target must be a class or an interface (or, for `as?` only, a numeric enum); testing against a struct, a primitive, a plain enum, an array, or a string is a compile error, and so is a nullable target like `as? Circle?`, whose result would already be nullable.
The scrutinee must be a class, an interface, or a nullable form of either, and the target must be related to it: a test that could never succeed (unrelated classes) and a test the static type already satisfies (always true) are both compile errors.
A nullable scrutinee tested against a type it already satisfies is the exception: for `Base? x`, the test `x is Base` is a combined null-plus-type check and is allowed.
An interface target over a class scrutinee is an error only in the impossible case, a `final` class that does not implement it (any other class could have an implementing subclass), or the always-true case where the static class already implements it.
An interface scrutinee may be tested against any class or interface target; the outcome is decided by the value's runtime type.

`if (x is Derived)` narrows `x` to `Derived` inside the branch, following the same rules as null narrowing above: the same paths narrow (locals, member chains, subscripts), `x is Derived && x.derivedMethod()` narrows the right side of `&&`, conjunctions narrow the branch, a loop condition narrows the body, and the same writes and calls drop the narrowing.
Failing the test proves nothing about the value's type, so the plain else branch of a positive `is` does not narrow.

Negating a check flips what it proves, so the fact that survives is the negated test and a negative guard narrows.
`!(x is Circle)` proves `x is Circle` on its surviving side, which is the else branch and the code following a guard that always exits, as in `if (!(x is Circle)) { return; }` where `x` is a `Circle` on every line below.
Negated null checks are symmetric: `!(x == null)` narrows like `x != null`, `!(x != null)` narrows like `x == null`, and double negation composes.

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

That default is held to the same rule as any other type, so a literal nothing gives a type to must fit the type it falls back to, and a value past it is an error naming the type that would hold it rather than a silent truncation.
A cast counts as naming a type: a literal whose value the target holds adapts to it, while one the target cannot hold keeps its default and is truncated by the cast, which is what a narrowing cast is for.

```ens
byte b = 5;              // OK - 5 fits in byte
long n = 5;              // OK - 5 fits in long
byte big = 300;          // error: 300 does not fit in 'byte' (range -128..127)
```

Floating-point literals follow the same rule in their own family: one written without a type suffix adapts to the surrounding type when that type is `float` or `double`, and with no context it defaults to `double`.
An unsuffixed integer literal adapts to a floating-point type too, since it names such a value just as well: `float half = 5;` holds `5.0`.
Rounding to the nearest value the type has is part of the conversion, so `float ratio = 0.1;` is accepted even though no binary floating-point type holds a tenth exactly, and `float count = 16777217;` stores `16777216`, the nearest value a `float` has.
Only a magnitude the type cannot hold at all, one that would become infinity, produces an error; a magnitude too small to tell apart from zero rounds to zero.
A suffix names the type and leaves nothing to adapt, so `1.0f` is a `float` and `1.0d` a `double`, just as `5L` is a `long`.

In both families a literal takes its type from what it sits against, and the surrounding type does not reach through an operator into its operands.
So `uint mask = 1 + 4;` and `float ratio = 1.0 + 2.0;` are both errors; give one operand the type you want, as in `float ratio = 1.0f + 2.0;` or `uint mask = (1 as uint) + 4;`, and the other adapts to it.

```ens
float ratio = 0.5;       // OK - the literal adapts to float
float tenth = 0.1;       // OK - rounds to the nearest float
double wide = 0.5;       // OK - a literal with no other context is a double
float suffixed = 1.0f;   // OK - written as a float outright
float tooBig = 3.5e38;   // error: too large for 'float', which holds about 3.4e38 at most
float viaOperator = 1.0 + 2.0;  // error: the operands are doubles, and their sum is a double
```

---

`let` and a typed declaration both introduce a mutable binding. `const` introduces an immutable one: it must be initialized, and assigning to it again, or passing it as `out`, is a compile error. Like `let`, a `const` may infer its type or state it explicitly.

A local variable need not be initialized where it is declared, but it must be definitely assigned before it is read: on every path that reaches a use of the variable, an assignment to it must come first.
A local has no implicit zero value, so this holds for every type, nullable or not: `int total; total = sum(xs); use(total)` is fine, while reading `total` before that assignment is a compile error.
Assigning in only some branches does not count, so after `if (c) { x = 1; }` the variable `x` is assigned only when the condition held, and a later read is an error unless every path assigns it.
Declaring a variable and never reading it is allowed; the rule governs reads, not declarations.

```ens
let count = 0;          // mutable, inferred int
count = count + 1;      // ok

const limit = 10;       // immutable, inferred int
const int max = 100;    // immutable, explicit type
limit = 11;             // error: cannot assign to constant 'limit'
```

A **compound assignment** folds an operator into the store: `a += b` means `a = a + b`, and the same holds for `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`, and `>>>=`.
The target is evaluated once, so in `xs[next()] += 1` the index `next()` runs a single time and serves both the read and the write.
Each form applies wherever its operator does: the arithmetic ones (`+= -= *= /= %=`) to numeric targets, the bitwise and shift ones (`&= |= ^= <<= >>= >>>=`) to integer targets, and `+=` also to a string, where it appends the right side just as `+` concatenates.
The two sides combine under the operator's own rules, and the operator's result must be assignable to the target, so a wider target takes a narrower value without a cast while a narrower target needs an explicit `as`.

```ens
int n = 10;
n += 5;                 // 15
n <<= 1;                // 30

long total = 0;
total += n;             // int widens to long, then adds
n += total;             // error: 'int' and 'long' produce 'long'

string greeting = "Hello";
greeting += ", world";  // "Hello, world"
greeting += 2;          // "Hello, world2": '+=' appends whatever '+' concatenates
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

The bitwise operators `&`, `|`, and `^` combine two integers of the same type and produce that type.
An untyped integer literal adapts to the other side, the same way it does for `+`, `-`, and `*`, so a `uint` masked with `0xFF` needs no cast.
The shift operators `<<` and `>>` move a value's bits by a distance, and `>>` keeps a signed value's sign while an unsigned one fills with zeros.
A third shift, `>>>`, always fills with zeros, so it differs from `>>` only on a signed type: `>>` copies the sign bit into the vacated places and `>>>` discards it.
On an unsigned type the two mean the same thing, because `>>` already fills with zeros there.
A shift produces the type of the value it shifts, so the distance must be that same integer type, and an untyped literal distance adapts to it.
The distance never decides the result, so an untyped literal on the left of a shift stays `int` and shifting by a `uint` distance is written `(1 as uint) << spread`.

The unary `~` flips every bit of an integer and produces the operand's own type; it sits with the other prefix operators and rejects anything that is not an integer.

```ens
uint flags = 0x12345678;
uint low = flags & 0xFF;        // 0x78: the literal adapts to uint
uint top = flags >> 24;         // 0x12: the distance adapts to uint
uint flipped = ~flags;          // 0xEDCBA987

int word = -8;
int kept = word >> 1;           // -4: '>>' copies the sign bit
int dropped = word >>> 1;       // 2147483644: '>>>' fills with zeros

byte mask = 0x0F;
byte bits = mask & 300;         // error: 300 does not fit in 'byte' (range 0..255)
bool ready = ~true;             // error: '~' flips the bits of an integer, got 'bool'
```

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

An enum member may be given an explicit integer value with `= value`, and an enum that assigns any of its members a value is a *numeric* enum.
In a numeric enum the first member must carry a value, and each later member without one continues from the previous member's value plus one.
Values may be negative or sparse, and every member's resulting value must be distinct; a collision, written directly or produced by continuation, is a compile error.

```ens
enum Errno {
    EPERM = 1,
    ENOENT,          // continues at 2
    EACCES = 13,
}
```

A plain enum, one that assigns no values anywhere, is a set of distinct names with no numeric identity, exactly as described above.
Assigning values does not change any of an enum's other behavior: numeric or plain, its members still compare by identity, drive `switch` by member, and print (and interpolate and serialize) as their member name.
The assigned value is only the backing number the member converts to and from.

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

Inside a string or `char` literal a backslash begins an escape.
The accepted escapes are `\n`, `\r`, `\t`, `\b`, `\f`, `\0`, `\\`, `\"`, `\'`, `\{`, `\}`, and `\uXXXX` for a Unicode scalar written as exactly four hexadecimal digits; any other escape, or a `\u` not followed by four hex digits, is a compile error.

- `==` and `!=` compare **contents**, not identity, so `"ab" == "a" + "b"` is true.
- `s.length` returns the number of UTF-8 **bytes** as a `long`.
- `+` concatenates strings. When one side is a string, a number (integer, `char`, or floating-point) or a `bool` on the other side is converted to text implicitly (the same way `.toString()` would). Types without a string conversion yet are still rejected.
- `.toString()` produces a string from a value explicitly: integer types format as decimal, floating-point types by the rule below, a `char` as the one character it denotes (its Unicode scalar encoded as UTF-8 bytes, so `'A'` is `"A"` and `'7'` is `"7"`, not their code points; write `c as int` first for the number), `bool` as `true` or `false`, a string returns itself, and a struct produces its JSON form.
  It can be written directly on a literal, as in `42.toString()`.
- `s.toBytes()` returns the UTF-8 bytes as a `byte[]`, and `string.fromBytes(bytes)` builds a string from a `byte[]` by interpreting it as UTF-8.
- `s.contains(needle)` reports whether `needle` occurs in `s`.
  `s.indexOf(needle)` returns the byte offset of the first occurrence as a `long`, or `-1` when absent; an empty needle is found at offset `0`.
- `s.startsWith(prefix)` and `s.endsWith(suffix)` report whether the first or last bytes of `s` are exactly that part; an empty part always matches, and a part longer than `s` never does.
- `s.trim()` returns `s` without the whitespace at either end, and `s.trimStart()` returns it without the whitespace at the start only.
  Whitespace is a space or one of the ASCII layout controls: tab, line feed, vertical tab, form feed, and carriage return.
- `s.substring(start, end)` returns the bytes in the half-open range `[start, end)` as a new string.
  Offsets are byte offsets, like `length` and `indexOf`.
  An invalid range (a negative start, a start past the end, or an end past the length) aborts the program.
- `s.compareTo(other)` returns an `int` that is negative when `s` sorts before `other`, zero when they hold the same bytes, and positive when `s` sorts after `other`.
  The order is the order of the UTF-8 bytes, and a string that is a prefix of another sorts before it.
  Only the sign of the result is meaningful.
  Strings have no `<`, `<=`, `>` or `>=` operators; write the comparison against `compareTo` instead.

A `float` or `double` renders as the shortest decimal text that reads back as exactly the same value, so no program has to know which digits a compiler chose to keep.
Concretely the text carries the fewest of 15, 16, and 17 significant digits that still reads back exactly, with trailing zeros left off: `2.5` is `"2.5"` and `0.1` is `"0.1"`, while `0.1 + 0.2` is `"0.30000000000000004"` because that sum genuinely is not three tenths.
A `float` renders through the same rule after widening to `double`, which is lossless, so both types print the same text for the same value.
A value with nothing after its decimal point loses the point too, so `3.0` is `"3"` and `0.0` is `"0"`; negative zero keeps its sign as `"-0"`.
Exponent form is used when the value's decimal exponent is below -4 or reaches the number of significant digits shown, and is written as `e`, a sign, and at least two digits: `1e-5` is `"1e-05"` and `1e21` is `"1e+21"`, while `0.0001` and `1234567` are written out in full.
The three values that are not numbers render as `"inf"`, `"-inf"` and `"nan"`, whatever the C library underneath would have called them.

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
let intro = greeting.startsWith("Hello"); // true
let clean = "  spaced  ".trim();    // "spaced"
if (name.compareTo("world") < 0) { /* name sorts first */ }
```

**Interpolation** embeds expressions in a string with `{ }`. Each hole is converted to text the way `.toString()` would, then the literal parts and holes are joined into one new string. Write `\{` and `\}` for literal braces.

```ens
let report = "Area: {width * height} for {width}x{height}";   // "Area: 100 for 20x5"
let status = "done={finished}, items={count}";                // bool and integer holes
let braces = "use \{these\} verbatim";                        // "use {these} verbatim"
```

Holes accept string, integer (including `char`), floating-point, `bool`, and enum values, and structs whose fields are all serializable (rendered as JSON); convert other types explicitly with `.toString()` first.
A `char` hole renders as its character rather than its numeric code point, so `"{'A'}"` is `"A"`; interpolate `c as int` when the number is wanted.
Inside a generic body a hole may hold a value of a type-parameter type; the requirement is then checked against the concrete type of each instantiation.

---

Native libraries can be called from Ens through `external` declarations. They are always written at the top of a source file, alongside `import`s and type declarations.

```ens
external type HANDLE;

external from kernel32 {
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

- `external type Name;` declares an opaque foreign handle. The handle is passed around and compared with `null`, but it has no members. It is private to its file by default and may be marked `public` to share it with the rest of the package; it can never be `export`ed, because a foreign handle has no meaning outside the package that binds it, so wrap it in an Ens type to cross a package boundary.
- `external from libname { ... }` groups foreign function signatures. The name is an identifier naming a native library declared in the package's `ens.package` manifest (here `native kernel32;`); using an undeclared name is an error, and the manifest's declaration tells the linker what to link (see the packages section). `libc` is the platform C runtime, declared `native libc system;` and linked by default. An external block and its functions are always private to their file and take no visibility modifier; share their behavior by wrapping the calls in Ens functions.
- The `out` modifier marks a parameter the C function writes back to. At the call site, the caller passes an initialized local variable as `out name`. The variable's type must match the declared parameter type exactly.
- A `string` argument is converted automatically to a NUL-terminated UTF-8 buffer at the call boundary. The C function must not retain that pointer past the call.
- To read a NUL-terminated C string that a C function hands back as a foreign handle, call `fromCString` from the `@std.ffi` module: it accepts a value of any external type and copies the bytes the handle points to into a new Ens string, returning `string?`. A null handle returns `null`, so an unset value stays distinct from an empty one. Freeing the C buffer stays the caller's responsibility.

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

The standard library is an external package, imported with `@`, and is opt-in apart from the implicitly imported `std.core`: its other exported declarations are visible only after they are imported. The `std.system` module wraps common operating system facilities and reports failures as exceptions. Import the module to call its functions through the `system` namespace, and import any types you use by name:

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

`system.exists(path)` reports whether a file or directory exists at the path, without throwing: only a missing path reports `false`.
`system.isFile(path)` and `system.isDirectory(path)` answer the same way for one kind each, so a missing path reports `false` from both.

`system.listDirectory(folder)` returns the names of everything directly inside `folder`: the names alone, without the folder in front of them and without the `.` and `..` entries, in ascending order by their bytes.
A walk over a tree therefore visits the same names in the same order whatever order the file system enumerates them in.
A folder that could not be read raises a `SystemError`.

`system.createDirectory(path)` creates the directory the path names together with every parent of it that is missing, and leaves a directory that is already there alone.
`system.createNewDirectory(path)` creates it the same way but answers whether this call is the one that created it: `true` when it did, and `false` when a directory was already there.
Every missing parent above it is still created the lenient way; only the directory the path itself names is claimed, so two programs asking for the same name at the same instant get different answers and exactly one of them may treat the folder as its own.
`system.removeFile(path)` removes one file and refuses a directory; `system.removeDirectory(path)` removes one directory, which has to be empty already, because what to do with what is inside it is the caller's decision.
Each of the four raises a `SystemError` when it does not succeed, and `createNewDirectory` raises one for a path where a file of that name is already there, since no folder can be claimed at that name.

`system.move(from, to)` moves the file or folder `from` names, together with everything under it, to `to`, and answers whether this call is the one that moved it: `true` when it did, and `false` when something was already at `to`.
The move happens in one step, so no other program ever sees half of it and two programs racing for the same name get different answers; that is also why both paths have to be on the same volume, since a move across volumes would have to copy.
A move that failed for any other reason raises a `SystemError`.

`system.currentDirectory()` returns the directory the program was started in, and `system.executablePath()` the path of the running program itself, both with `/` between their parts on every platform, so the `@std.path` functions read them as they are.
Neither can be changed: a program builds the paths it works with rather than moving itself somewhere else.

`system.environment()` returns the variables the program inherited as `NAME=VALUE` entries in ascending order by their bytes, and `system.getEnvironmentVariable(name)` returns one variable's value, or `null` when it is not set.
The environment cannot be changed either; a program that wants a child to see something else passes it when it starts that child.

`system.platform()` names the system the program was built for as `"windows"`, `"linux"` or `"macos"`, the same three names a package manifest uses for a native binding.
Any other system the compiler can target reports `"linux"`, whose behavior it shares.

`system.writeError(message)` writes `message` and a newline to standard error, the stream a program reports its problems on, the way `print` writes to standard output.

What `print` writes is buffered, so a line it wrote may still be inside the program when the next statement runs, but the order a program can observe is guaranteed: everything printed before a `system.writeError`, before a child process starts, and before a panic is reported has left the program first, whichever stream those go to and whether the streams are a terminal or the same file.
A panic therefore never loses what was printed before it.
`system.flush()` hands over what is still buffered on demand, for an order only the program itself knows about.

`system.run(command)` runs one command line through the operating system's command interpreter.
`system.run(program, arguments)` starts `program` with `arguments` and waits for it, leaving both output streams with the running program, so what the child writes appears where it would have appeared as it is written.
`system.runCaptured(program, arguments, stdoutPath, stderrPath)` writes what the child sent to its standard output and its standard error into the two files instead, and an empty path leaves that one stream alone, so one call can capture one stream and pass the other through.
Both have a further form taking a `string[]` of `NAME=VALUE` overrides after the arguments: those entries are laid over the environment the program inherited, replacing the entries naming the same variable and leaving every other variable in place, so a child never loses the search path it needs to find the programs it runs.
Two names match without regard to case on Windows, the way that system matches them, and exactly everywhere else, where `PATH` and `path` name two different variables and both reach the child.
No command interpreter takes part in these forms, so every argument reaches the program exactly as written, whatever spaces or quotes it holds, and nothing is expanded on the way.
Each returns the child's exit code; a program that could not be started reports `127`, the code a shell reports for the same failure, and a capture file that could not be opened for writing raises a `SystemError`.

```ens
import @std.system;

int status = try system.runCaptured("git", ["commit", "-m", "a message with spaces"],
    "build/git.out", "build/git.err");
int built = try system.run("make", ["all"], ["CC=clang"]);
```

`system.start(program, arguments)` starts `program` the same way but hands back a `ChildProcess` while it is still running, so this program reads what the child writes as it is written instead of after it has finished.
It has the same further form taking a `string[]` of `NAME=VALUE` overrides after the arguments.
`readLine()` returns the next line of the child's output without its ending, waiting as long as the child takes to write it, and `null` once the output has ended; output that does not end in a newline is a line of its own, handed over last, and a carriage return before a newline belongs to the ending.
What the child sent to its standard output and what it sent to its standard error arrive together, in the order it wrote them, which is what a program relaying another program's output needs; use `runCaptured` when the two have to stay apart.
`waitForOutput(milliseconds)` reports whether `readLine()` can go ahead without waiting for the child: `true` when a whole line is there to be had, and `true` once the output has ended, because reading then answers `null` at once.
`false` means that many milliseconds went by with neither, and the child may still be about to write something; a bound of `0` asks about this moment alone.
It is what a program that must not wait forever on another one asks before reading, since reading itself waits for as long as the child takes.
`kill()` stops the child, whose output then ends where it was stopped and whose exit code is `137`, the code a system reports for a program ended from outside, unless the child had already finished on its own and reported a code of its own.
Stopping a child that has already finished is not a failure, so a program that stops one it has given up on need not first find out whether it is still there.
`wait()` returns the child's exit code, waiting for the child to finish first, and answers the same code however often it is asked.
Output that has not been read is read and thrown away before that wait, because a child whose output nobody reads would be stopped by a full pipe: read every line first where the output matters.
A child that was stopped is not read out first, so asking a stopped child for its code always comes back.
Letting a `ChildProcess` go without waiting for it hands back the pipe its output was arriving on and this program's hold on the child, which keeps running with nothing left reading it.
A program that could not be started at all raises a `SystemError` on the systems that report that while the child is being created, and elsewhere shows up as a child that ends with `127`.

```ens
import @std.system;
import ChildProcess from @std.system;

ChildProcess child = try system.start("git", ["clone", url]);
string? line = try child.readLine();
while (line != null) {
    print(line);
    line = try child.readLine();
}
int status = try child.wait();
```

The `@std.path` module works on paths as text, with `/` separating the parts on every platform, and never looks at the file system.
`join(base, relative)` puts one separator between two parts, and gives back the relative part alone when the base is empty.
`parentFolder(path)` returns the folder one level up: `""` when the path names no folder, and `/` at the root.
`fileName(path)` returns the last part of the path.
`extension(path)` returns the text after the last `.` of that last part without the dot, and `stem(path)` returns the part before it, so `notes.txt` reads as `notes` and `txt`.
A name with no `.`, and one whose only `.` starts it the way a hidden file is named, has no extension and is its own stem.
`normalize(path)` drops the `.` parts and the repeated separators, and resolves every `..` that has a part before it to remove.
`isAbsolute(path)` reports whether the path names a place on its own, which a path starting at a root does, and so does one starting at a drive letter the way Windows writes one.
`absolute(path, workingDirectory)` reads a relative path against that folder and normalizes the result, leaving an already absolute path only normalized.
The folder is given rather than read from the running program, which is what keeps every function in this module a rule about text.
Because every function here works in `/` form, `fromNative(text, platform)` is the conversion at the boundary, where a path written the way an operating system writes one arrives.
On `"windows"` it turns each `\` into `/`; on every other platform it gives the text back unchanged, because `\` is an ordinary character in a name there and converting it would name a different file.
The platform is named by the caller rather than read from the running program, so either platform's answer can be asked for anywhere.

---

Every value has a `hash()` method returning a `long`. Value types (primitives, enums, strings, structs) hash by their contents, so equal values hash equally; classes and arrays hash by identity, matching how `==` compares them.
An optional hashes as its payload does while it is present and as one fixed value once it is absent, so every absent value hashes equally whatever its type.
A class or a struct can declare its own `hash() -> long` to control its hashing, paired with `equals(T other) -> bool` - a method taking a single parameter of the declaring type `T` itself - to control equality.
A method named `hash` must have exactly that signature, and neither `hash` nor `equals` can be `throws`, because the language takes a value's hash and compares two values where there is no room for a `try`; `equals` must return `bool`.
When a class declares such an `equals`, `==` and `!=` on that class compare by content - an identity and null check first, then `equals` - rather than by reference identity; when a struct declares one, `==` and `!=` call it instead of comparing the fields.
Both `hash` and `equals` are written with `override`, since they replace behavior the language provides: a class's identity hash and equality, a struct's content hash and memberwise equality.
The two are a matched pair: a type that declares one must declare the other, so equal values always hash equally.
A declared `hash` decides the hashing of its type everywhere the value appears, including as a field of an enclosing struct, as an array element, and behind a `Hashable` bound; a declared `equals` decides `==` the same way.
The `Hashable` interface from `@std.hash` names the hashing contract for generic bounds, and every type satisfies it.
Because the language calls `hash` and `equals` wherever the type is used, both follow their type's visibility when unmarked and may not be marked less visible than the type itself.

For a class, which implementation runs is decided by the value's type at run time, not by the type written in the source.
An object of a class that declares `hash` and `equals` keeps them when it is held in a variable, field, array or collection typed as a base class or as an interface, so two such objects that are equal as their own class stay equal and hash alike where the code holding them only knows the base type.
Because a class's `equals` takes its own class, two objects compare by content only when their run-time types are the same one; objects of different run-time types are never equal, even when one class inherits the other's `equals`.
A class that declares neither method - including a base class whose subclasses declare them - keeps identity equality and the identity hash for objects of exactly that class.

The collection modules build on hashing and iteration:

- `List<T>` from `@std.collections.list` is a growable array: `push(value)`, `pop()` removing and returning the last value, `get(index)`, `set(index, value)`, and `length()`, plus `toArray()` returning a fresh right-sized `T[]` holding the current contents. Iterating a list yields its values in insertion order.
- `Map<K, V>` from `@std.collections.map` maps keys to values: `set(key, value)` inserts or overwrites, `get(key)` returns `V?` (`null` when absent), plus `contains(key)`, `remove(key)`, `length()`, and `keys()` / `values()` snapshots. Iterating a map yields `Pair<K, V>` entries (from `@std.collections.pair`) with `key` and `value` fields.
- `Set<T>` from `@std.collections.set` stores each value once: `add(value)` returns whether the value was new, plus `contains(value)`, `remove(value)`, `length()`, and `items()`. Iterating a set yields its values.
- `sort(values)` from `@std.collections.sorting` puts a `List<string>` in ascending order in place, by the order `compareTo` defines. The sort is stable.

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

Keys are matched with `==` and bucketed with `hash()`: strings by contents, value types by value, and classes by identity - unless a key's run-time class declares `equals` (with its paired `hash`), in which case that key matches by content.
A map or set keyed by a base class or an interface therefore finds the entry a derived key stored, because the key's own class decides how it is matched and bucketed; two keys of different run-time classes never match.
Struct keys are supported and match by content: their fields compare with `==` and hash by content, so a key rebuilt from equal field values finds the entry stored under the original.
A struct key that declares its own `equals` and `hash` is matched and bucketed by that pair instead, so a field the pair ignores does not change which entry a key finds.

The `@std.text.strings` module takes text apart and puts it back together.
`split(text, separator)` returns the parts between the occurrences of the separator, so two neighboring separators give an empty part and text holding none gives one part.
`lines(text)` splits on `\n` and drops a trailing `\r` from each line, so text written with either line ending reads the same.
`joined(parts, separator)` puts the separator between neighboring parts and nothing before the first or after the last.
`parseInteger(text)` returns the `long` the text spells, or `null` when it spells anything else: at most one leading `-` or `+`, then nothing but digits, and a value a `long` can hold, so surrounding whitespace and a value too large are refused rather than guessed at.

```ens
import @std.text.strings;

string[] fields = strings.split("name,age,city", ",");
long? count = strings.parseInteger("42");
print(strings.joined(fields, " | "));
```

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
