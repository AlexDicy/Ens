`TODO`: add visibility to allow only some classes and descendents to access a protected method/member.
`TODO`: field aliases/getters, for example `Vector4` struct can have `x`, `y`, `z`, `w` and `r`, `g`, `b`, `a`.

Primitive types: `bool (1)`, `byte (1)`, `short (2)`, `ushort (2)`, `int (4)`, `uint (4)`, `long (8)`, `ulong (8)`, `float (4)`, `double (8)`

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

    // the constructor can be a full method or a shorthand which initializes the fields. Methods can have optional parameters, optional parameters must provide a default value. This syntax allows to use either new Animation(); or new Animation(myShape);
    // the default expression must be assignable to the field's declared type.
    Animation(this.shape = null);

    // or:
    // Animation(S? shape = null) {
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

Importing from packages follows the format `@packageorg.packagename.path`.

```ens
import @std.fs.file; // for external dependency /src/std/fs/file.ens
import Observable from @alexdicy.reactivity.observable; // for class Observable in external dependency /src/alexdicy/reactivity/observable.ens
```

Methods that can throw exceptions are marked with `throws`; any other method can be considered safe. The list of throwable types is computed and not part of the method signature. IDEs will infer and show them on hover.

If any exception is not handled and the method is not marked as `throws`, this should result in a compilation error explaining which exceptions were not handled and how to handle them (either with a `catch` block or via the `throws` keyword).

These exceptions are always checked. The user can use `panic()` to stop execution, which doesn’t require the use of the `throws` or `throw` keyword.

Methods that throw must be called with `try` as a prefix, even if caught. `Catch` can be added as an additional block of a method.

`Finally` blocks run last and are optional.

```ens
class TestRepository {
    getName() -> string? {
        return try queryName();
    } catch (DatabaseError e) {
        Log.warn("Database error occurred: {e}");
        return null;
    } finally { // or other catch blocks if multiple exceptions are thrown
        Log.debug("getName() called");
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
---

A type written without a `?` always holds a value and can never be `null`. To allow `null`, suffix the type with `?`.

```ens
class Inner { /* ... */ }
class Outer {
    Inner? inner;     // may be null
    Outer(this.inner = null);
}
```

To read through a nullable value, use the safe member operator `?.`. If the value on the left is `null`, the whole expression evaluates to `null` and the right-hand side is not evaluated; otherwise it behaves like `.`.

```ens
Outer? outer = new Outer();
Inner? maybeInner = outer?.inner;   // either null or the field value
```

Inside `if x != null { ... }` the `x` is considered as the non-nullable form for the rest of the block, so you can use `.` directly. The same narrowing applies to the `else` branch of `if x == null { ... } else { ... }`. Reassigning `x` inside the block drops the narrowing from that point on.

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

---

Arrays are written with `T[]` and are reference types: declaring an array variable binds a pointer to a heap allocation, and copying the variable copies the pointer.

```ens
int[] xs = new int[5];        // 5 ints, zero-initialized
xs[0] = 10;
xs[1] = xs[0] * 2;

let n = xs.length;            // long
```

- `new T[size]` allocates an array of `size` elements. Primitive and reference slots start zero / `null`. Struct slots get the struct's declared field defaults applied to each slot.
- The element type must be one whose default value is meaningful. A non-nullable reference type (class, array, external, string) is rejected. Use the nullable form instead: write `Box?[]` rather than `Box[]`. The same rule extends through struct fields: a struct containing a non-nullable reference field cannot be used as an array element.
- `arr[i]` reads or writes an element. Bounds are checked at every access; an out-of-range index aborts the program.
- `arr.length` returns the number of elements as a `long`.
- `T?[]` is an array of nullable `T`, each **element** can be `null`. `T[]?` is a nullable array variable. The variable itself may be `null`. The two compose: `T?[]?` is both.

```ens
int[]? cache = null;
if (cache != null) {
    cache[0] = 1;             // `cache` is `int[]` here
}
```

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
    // no retain at entry, no release at exit — caller's reference owns +1
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
