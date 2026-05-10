`TODO`: add visibility to allow only some classes and descendents to access a protected method/member.
`TODO`: field aliases/getters, for example `Vector4` struct can have `x`, `y`, `z`, `w` and `r`, `g`, `b`, `a`.

Primitive types: `bool (1)`, `byte (1)`, `short (2)`, `ushort (2)`, `int (4)`, `uint (4)`, `long (8)`, `ulong (8)`, `float (4)`, `double (8)`
Additional primitives if possible: `decimal (16?)`

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
