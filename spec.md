`TODO`: add visibility to allow only some classes and descendents to access a protected method/member.
`TODO`: field aliases/getters, for example `Vector4` struct can have `x`, `y`, `z`, `w` and `r`, `g`, `b`, `a`.

Primitive types: `bool (1)`, `byte (1)`, `short (2)`, `ushort (2)`, `int (4)`, `uint (4)`, `long (8)`, `ulong (8)`, `float (4)`, `double (8)`, `char (4)`. `byte` is unsigned (0..255); `short`/`int`/`long` are signed; `ushort`/`uint`/`ulong` are their unsigned counterparts.
`char` is an unsigned 32-bit Unicode scalar value (0..0x10FFFF); it counts as an integer type, and converts to text as the character it denotes.
Being a code point rather than a quantity, a `char` converts to `int`, `uint`, `long` and `ulong` on its own, since each of them has room for every code point, but never `float` or `double`: arithmetic on a code point in integers is meaningful, while a code point as a floating-point number is a mistake rather than an intent.
Write `c as double` where the number behind the character is what is wanted.
`byte`, `short` and `ushort` have room for only some code points, so a `char` converts to them only through `as`, as in `c as byte`.

Visibility has three tiers, `private` < `public` < `export`, with `protected` alongside them.
Everything is private by default.
An unmarked top-level declaration is visible only within its file.
An unmarked member of a `public` or `export` class or struct, including a constructor, is visible only within its type.
An unmarked member of a file-private class or struct, including a constructor, is visible throughout the type's file, which is exactly as far as the type itself reaches.
A class that is not `abstract` and declares no constructor can be built with `new` and no arguments wherever the class itself is visible.
`public` makes a declaration visible to every module in the same package; other packages cannot see it.
`export` makes a declaration visible to the packages that consume this one through `@` imports, including programs using `@std`.
`protected` keeps its own meaning for class and struct members: the declaring file, plus subclasses in the case of classes.
Top-level `protected` is not allowed.
A `public` or `export` class or struct never passes its visibility on to its members: they stay private to the type unless marked.
`protected` on a member of a file-private type reaches the same places as no marker at all.
`private` is never written; an unmarked declaration already has the narrowest visibility its place allows, and writing the word is an error.
A member private to its type belongs to that type alone, so it is not inherited: a subclass cannot call it, cannot override it, and may declare a method of its own under the same name with no relation to the base's.
The same holds for a field: a subclass may declare a field whose name a base field private to its type uses, the two hold storage of their own, and each class's code reaches the one it declared.
An unmarked member of a file-private class is not private to it, so a subclass, which shares its file, inherits that member: it may override such a method, and it may not declare a field under such a field's name.
Overriding a base method therefore requires that method to reach its subclasses, by being `protected` or wider or by being unmarked in a file-private class, and an abstract method must reach them the same way, since a subclass has to implement it.
A method that provides an interface requirement must be as visible as its own class, because anyone who can hold one of its values as the interface can call it through the interface.
A method that overrides a base class method writes the visibility of the method it overrides, capped at what its own class can hold (`protected` in a file-private class, `public` in a `public` one), and any other marker is an error.
An override with no marker is an error too, except in a file-private class, where an unmarked member already reaches as far as `protected` does.
One kind of method follows its type's visibility instead of the default: a method that replaces a behavior the language already provides, meaning a struct's or a class's `toString`, `hash`, and `equals`, where the `toString` is one that takes no parameters.
The language calls such a method wherever the type is used, so it may not be marked less visible than its type either.
Interface members carry no visibility of their own: they always follow the interface, and writing a visibility modifier on an interface member is an error.
Enum cases follow their enum.
A member may not be declared more visible than the type that contains it: an `export` method on a `public` class, or a `public` field of a file-private class, is an error, never a silent cap.
A declaration's signature may not mention a type less visible than the declaration itself; this covers parameter types, the return type, declared thrown types, field types, a base class, implemented interfaces, and generic arguments and bounds.
A protected member is held to the same rule at the widest scope its class can be subclassed from: the file for a `final` or file-private class, the package for an open `public` class, and everywhere for an open `export` class, whose external subclassers must be able to name every type its protected members mention.
For a `sealed` class, that scope is the widest one any of its subclasses can be subclassed from, and its own file at least.
So the protected members of a sealed class whose subclasses are all `final` stay within its file, while one open `public` subclass widens them to the package.
A `test` declaration sees its file's private top-level declarations and the unmarked members of its file-private types like any other code in the file, but not a member private to its type.

```ens
public calculateArea(uint width, uint height) -> uint {
    return width * height;
}

printArea() { // private by default; no need to specify -> void
    uint width = 20;
    uint height = 18;
    let area = calculateArea(width, height);
    print("Calculated Area: {area}");
    area = calculateArea(height: 56, width: 90);
    print("New area: {area}");
}
```

A parameter, a field, or a variable of type `void` is an error, because `void` means no value.
A call to a function that returns nothing has no value, so using it where a value is needed, such as an initializer, an argument, an operand, a condition, an interpolation hole or a return value, is an error.
A function with a non-void return type must return a value on every path through its body: the compiler rejects a function that can reach the end of its body without hitting a `return`, `throw`, `rethrow`, or a call that never returns (`panic()` or any `noreturn` function).
An `if`/`else` where every branch exits counts as exiting, as does a `switch` whose arms all exit.
A loop with no `break` that leaves it counts as exiting when its condition always holds: a `while` or `for` whose condition is the literal `true`, in any number of parentheses, or a `for` with no condition.

A function or method may be declared `noreturn`, a modifier stating that it never returns to its caller: every path through its body ends by throwing, by calling `panic`, or by calling another `noreturn` function, or the body loops forever.
A `noreturn` declaration has no return type, since it returns nothing at all, so writing a `-> T` clause alongside it is an error; it combines with visibility, `override`, `final`, and `throws`, but is not allowed on a constructor, a destructor, or a test.
The compiler enforces this on the body: a `return` statement is rejected, and a body that can reach its end is rejected, reusing the same path analysis as the missing-return check.
A call to a `noreturn` function ends the path it sits on, exactly as `panic()` does, so a value-returning function may close a branch with such a call and still be accepted, and a value narrowed before the call stays narrowed afterward.
Such a call gives no value, but nothing after it runs, so it may stand where the program states the type of the value read there, and it takes that type.
Those places are the initial value of a variable, an assigned value, an argument, a parameter's default value, a return value, a lambda's result, a field's value and an element of an array whose type is written.
The value of an assignment with an operator, such as `total += stop();`, and the right side of `??` are such places too.
So is any element after the first of an array whose type is not written, because the first element gives that array its type.
As an argument it fits every parameter, so it neither picks one overload over another nor shows what a type argument is.
A variable declared with `let`, or with `const` and no type, may take such a call as its initial value too, and it is never created.
Anywhere else, such as an operand of `+`, before a `.`, as an index, as a condition or in an interpolation hole, using it is an error, because nothing there states the type it would take.
It may also be an arm of a switch used as a value or a branch of `?:`, where it takes no part in the common type, so the other arms decide the type of the whole, as in `long level = switch (shade) { Light -> 1, Dark -> panic("dark"), };`.
When every arm or branch never returns, the whole switch or `?:` never returns either, and it counts as such a call wherever it is written.
The built-in `panic` is itself a `noreturn` function, so this is one rule rather than a special case.
An abstract or interface method may be `noreturn` as part of its contract, and an override of a `noreturn` method must remain `noreturn`, because callers rely on it never returning.
A throwing `noreturn` function still takes part in checked exceptions as usual: its call sites need `try`, and its declared or inferred `throws` set applies normally.

```ens
public noreturn fail(string message) throws {
    throw new TestFailure(message);
}
```

Structs automatically implement `copy()` and are `serializable` by default, which also allows printing them as JSON strings. Private and protected fields are included in the serialization.

```ens
struct Rectangle {
    protected uint width;
    protected uint height;
    string name = "unnamed rectangle ({width}x{height})"; // automatically initialized in order, after width and height have initialized. Defaults are evaluated in declaration order; a default may only reference fields declared earlier.

    getHalfArea() -> double {
        return (this.width * this.height) / 2;
    }
}
```

A struct value is built with a context-typed aggregate literal or a constructor call.

An aggregate literal `{field: value, field: value}` names each field it sets, and takes its type from the surrounding context: the declared type of a variable, a parameter, a return type, an assignment target, or an array element.
That context reaches a literal written as a branch of `?:` or an arm of a switch, and where there is none, such a literal takes the type of the other branches (see the switch section).
Every field that has no declared default must be listed; a field that has a default may be omitted to accept that default.
Naming a field the struct does not have, listing the same field twice, leaving out a required field, or giving a value that is not assignable to the field's type are each errors.
A field's visibility is respected: a literal sets a field only where the field is visible, the same rule as a direct field assignment.
A literal with no context to infer its type from, such as `let p = {x: 1};`, is an error; annotate the target or use a constructor.

A struct may declare a `constructor` with the same keyword and shorthand as a class, and it is invoked by writing the struct's name followed by arguments, for example `Point(1, 2)`.
Construction is by value and never uses `new`, which stays reserved for classes and arrays.
A generic struct is constructed the same way, with the type arguments written on the name: `Pair<int, string>(1, "x")` resolves the constructor on that instantiation, just as `Pair<int, string> value = {key: 1, value: "x"}` builds the instantiation from a literal.
The two forms coexist: a struct that has a constructor can still be built from a literal.

A struct local must be built as a whole, with a literal or a constructor, before any field of it is read or written.
`Point p; p.x = 1;` is an error because `p` is used before it holds a value; write `Point p = {x: 1, y: 2};` and then mutate a field of the already-built value.

A struct field may have any type, including a non-nullable one such as a class, a string, an array, or another struct that has no default.
A struct with such a field has no default value of its own, so it cannot be an array element or a field left without a default, though it can still be built with a literal or a constructor wherever a value is needed.
A `const` field without a default value removes the struct's default the same way, whatever the field's type: a default-built value would hold a zero that no constructor or aggregate literal ever assigned.

Two values of the same struct type compare with `==` and `!=` field by field, in declaration order, stopping at the first field that differs.
Each field compares by its own `==`: primitives and enums by value (so IEEE rules hold, and a `float` or `double` field that is `NaN` never equals itself), strings by content, class fields by reference identity unless the two objects' run-time class opted into content equality, `external` handle fields by identity, array fields element by element, a nested struct memberwise unless it declared its own equality, and a nullable field null-aware (both `null` are equal, one `null` is unequal, otherwise the inner values compare).
Comparing two different struct types is an error, and so is comparing structs whose type has a field with no `==` of its own, such as a function value or an array of them; the error names the offending field.
Inside a generic body, a field whose type a type argument decides, such as `T` or `T[]`, is judged against the concrete type of each instantiation, while a field with no `==` for any type argument, such as one of type `(T) -> int`, is refused in the generic body itself, and no instantiation reports it again.
A struct customizes equality by declaring `equals(S other) -> bool`, a method taking a single parameter of the struct's own type `S`, which then defines `==` and `!=` for that struct everywhere it is compared, including as a field of another struct, as an array element and as a collection key.
Such an `equals` replaces the memberwise comparison the language provides, so it is written `override`, and it must be paired with an `override hash() -> long`: a struct that declares one must declare the other, exactly as a class must, so equal values always hash equally.
A method named `equals` whose single parameter is some other type is an ordinary method, and `==` on that struct stays memberwise.

A struct serializes to a JSON string through `.toString()` and in interpolation holes, since structs are serializable by default.
The form is a JSON object listing every field, including private and protected ones, in declaration order: `{"field": value, ...}`.
Numbers render as decimals, `bool` as `true` or `false`, strings and enum members as JSON-quoted text (with `"`, `\`, and control characters escaped), a `char` as a one-character quoted string with its scalar encoded to UTF-8 and escaped the same way, an absent nullable field as `null`, and a nested struct as its own JSON object.
A struct that declares a `toString` taking no parameters uses that method instead; because it replaces the built-in form, it is written `override toString() -> string` and never `throws`, since an interpolation hole has nowhere to write a `try`.
A struct is serializable only when every field is: a value type, a string, an enum, one of those made nullable, a nested such struct, or an array of any of these, which serializes as a JSON list.
A field that is a class or an external handle has no JSON form, and neither does an array whose elements are; such a field makes serializing the struct an error that names it, mirroring the `==` rule.

A struct may declare `implements` and name any interface, which is how a struct satisfies a generic bound: a `<T: Comparable<T>>` parameter accepts `Path` once `Path` is declared to implement `Comparable<Path>`, so `paths.sorted()` and `SortedMap<Path, V>` work over a value type.
A struct cannot use `extends`, because a struct has no base type.
The conformance is checked where it is declared: the struct must provide every method the interface, and every interface it extends, requires, with a matching signature and the `override` marker, exactly as a class implementing the same interface does.
A method that implements a requirement must be as visible as the struct itself, since the generic body behind the bound calls it wherever the struct can be named.
A conditional member cannot satisfy a requirement, because a requirement has to exist for every instantiation.
That conformance is a compile-time judgment and nothing more, because a struct value is never wrapped in an object.
Storing a struct in an interface-typed variable, field, array element, or parameter is an error, and so is `is` or `as?` against an interface; each message names the bound the conformance does serve.
A call on a bound type parameter therefore reaches the struct's own method directly, with no allocation and no dispatch.
A struct that implements `Iterable<T>` can be walked by a for-each loop for the same reason, since the loop calls `makeIterator()` on the struct itself.
A value of a type parameter bounded only by interfaces cannot be stored where one of those interfaces is expected, because the type argument can be a struct.
Bound the parameter by a class that implements the interface, and the value is then an object of that class.

A struct method that changes the struct it is called on is marked `mutating`, written with the other modifiers before its name, as in `mutating bump() { this.count += 1; }`.
A method changes the struct when it writes a field of `this` at any depth, with `=`, a compound assignment, `++`, or `--`, or when it calls a `mutating` method on `this` or on a struct field of `this`.
Writing an element of an array that a field holds is not a change to the struct, because an array is a reference.
Such a method without the marker is an error, while marking a method that changes nothing is allowed.
A `mutating` method is called only on a value that may change, so a call on a `const` local, a `const` field, a `lazy const`, a struct a lambda captured, the variable of a for-each loop, `this` in a method that is not `mutating`, or a struct a call returns is an error.
An interface method may be marked `mutating`, a struct that implements it marks its method exactly when the interface does, and a call through a generic bound follows the interface's marker.
A class implementing that interface writes no marker, because a class method changes its object through a reference.

A `mutating` method called through `?.` on an optional struct changes the struct where the optional is kept, and does nothing when the optional is null.
When the optional holds a value, the arguments are evaluated first, and the method is called only if it still holds one after them.
An optional struct on the way to it, such as `outer` in `outer?.inner?.bump()`, is checked again after the arguments in the same way.
A class or array reference on the way, such as `holder` in `holder?.maybe?.bump()`, is not, because the object it names is already held, so the method runs on that object even when an argument sets the reference to null.
That call is an error on the same values as one written with `.`, at every link of a chain such as `holder.outer?.inner?.bump()`.
An optional kept in a local variable or a parameter, directly or inside a struct, changes in place, exactly as a struct does when its method is called with `.`.
An optional kept anywhere else, such as in a field or an array element, changes through a copy that is stored back when the method returns or throws, so the call differs from one written with `.` in these ways:

- Code that reads the optional by any means other than `this` while the method runs sees the value from before the call.
- When the method returns or throws, the copy replaces what the optional holds, so a change made to the optional by any means other than `this` during the call is lost.
- When the optional, or a struct on the way to it, has been set to null by then, the copy is discarded and nothing is stored back.
- When a struct on the way to the optional, such as the `Outer` that `holder.outer` holds, is replaced during the call, the copy is stored into the replacement.

```ens
Counter? counter = {count: 0};
counter?.bump();               // counter now holds a count of 1
holder.outer?.inner?.bump();   // changes 'inner' inside the 'Outer' that 'holder.outer' holds
```

Overloading is allowed, best match arguments first, then visibility.
Two declarations of the same name must differ in parameter count or parameter types.
A call picks the overload whose parameter types match the arguments exactly; when there is no exact match, an overload reachable through implicit widening is chosen.
A lambda fits only a parameter of function type that takes as many parameters as the lambda declares.
The name of a method or a function fits a parameter of function type only when some declaration of that name, generic ones included, can be called with as many arguments as that function type takes.
An array literal first fits by the type it has alone, an array of its first element's type.
Only when no overload takes that type is it fitted element by element, and then it fits as well as its worst-fitting element.
An empty `[]` fits every array parameter.
A struct literal fits only a parameter whose type is a struct or a nullable struct, and it fits every such parameter as a value of that struct would, whatever fields it names.
A `?:` or a switch that holds one of these arguments as a value fits a parameter as well as its worst-fitting value does.
Once an overload is chosen, each of these arguments is typed from the parameter it maps to, and an argument that parameter cannot type is an error.
A call evaluates and checks its arguments in the order they are written, including the ones the chosen overload types.
A lambda among them sees what holds where it is written: in `run(() -> held.length, held = "abc")` the lambda copies `held` before the assignment runs, so the narrowing the assignment makes does not reach it.
A name is not a value, so a name that picks an overload is an error whose fix is a lambda that calls it, and a name that picks none is an error of its own.
A lambda or an array literal that two overloads fit equally well, such as a lambda where both take a function type with the same number of parameters, leaves the call ambiguous.
When two overloads match equally well, the call goes to the more specific one.
A parameter type is more specific than another when every value of it converts implicitly to the other and not the reverse.
So `Circle` is more specific than `Shape`, `Circle?` and `Shape?`.
A class is more specific than an interface it implements, and an interface than one it extends.
A number is more specific than one it converts to without `as`, as `short` is than `int` and `int?` than `long?`.
An overload is more specific than another when the type of each parameter an argument fills is the same or more specific, and at least one is more specific.
A `Square` that extends `Circle` therefore calls `describe(Circle c)` rather than `describe(Shape s)`.
How well the arguments fit is decided first, so for that `Square`, `fit(square, 1)` calls `fit(Shape s, int n)` rather than `fit(Circle c, long n)`, because the literal `1` fits `int` better than `long`.
Two types where neither converts implicitly to the other are not comparable, so `m(long)` and `m(double)` stay ambiguous for an `int`, because a `long` does not convert implicitly to a `double`, and so do `k(Circle? c)` and `k(Shape s)` for that `Square`.
Where a generic type's type argument gives two overloads the same parameter types, the one written without the type parameter is more specific: `Box<Shape>` calls `put(Shape value)` rather than `put(T value)`.
When neither overload is more specific, the call is a compile error that lists the candidates.
Overloads that are not visible from the call site lose to visible ones; visibility is only an error when no visible overload matches.
Named arguments participate in selection: an overload is only considered when every named argument names one of its parameters.
A subclass may add a new overload of an inherited method name; `override` still requires the exact signature of the method it replaces.
External functions and generic functions cannot be overloaded.

```ens
calculateArea(Rectangle rectangle) -> uint {
    uint area = rectangle.width * rectangle.height;
    print("Calculated Area: {area} for rectangle: {rectangle}");

    // example: calculateArea({width: 20, height: 5});
    // outputs: Calculated Area: 100 for rectangle: {"width": 20, "height": 5, "name": "unnamed rectangle (20x5)"}

    return area;
}
```

```ens
class Animation<S: Shape + Comparable<S>> {
    S? shape;

    // A constructor is introduced by the `constructor` keyword. It can be a full method or a shorthand which initializes the fields. Methods can have optional parameters, optional parameters must provide a default value. This syntax allows to use either new Animation(); or new Animation(myShape);
    // the default expression must be assignable to the field's declared type.
    constructor(this.shape = null);

    // or:
    // constructor(S? shape = null) {
    //     this.shape = shape;
    // }

    start() -> bool {
        print("Has shape? {this.shape != null ? "Yes" : "No"}.");

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
public class Shape {
    protected int sides;
    protected constructor(this.sides = 0);
    public area() -> int { return 0; }
    public describe() -> string { return "a shape with {this.sides} sides"; }
}

public class Square extends Shape {
    int side;
    constructor(int s) {
        this.side = s;     // assign the subclass's own fields first
        super(4);          // then construct the base class
    }
    public override area() -> int { return this.side * this.side; }
}
```

Methods are overridable by default. An override must be marked `override` and must match a method declared in a base class; this catches typos and accidental shadowing. Mark a method or a class `final` to forbid overriding or extending it.
An override also carries the visibility marker of the method it overrides, as the visibility section describes, so `protected override sound()` replaces a `protected sound()`.
An override's parameter types must match exactly, while its return type may be narrower where a value of it is held the same way: a class, interface, array, string, function or external handle reference, or one of those with its `?` dropped, because both spellings are one reference.
A narrower number, a nullable value type with its `?` dropped, and one level dropped from a doubly nullable type are each rejected, because a caller reaching the method through the type that declared it would read the declared shape and find the other one.
The same rule governs the method a class or a struct provides for an interface requirement.
`override` on a method that overrides nothing is an error, so the marker always names something real: a base or interface method, or a behavior the language provides for the type, which means `toString`, `hash`, and `equals` for a struct and for a class alike.
The marker is required wherever a declaration has the shape the behavior needs, on a struct and on a class alike, so a `hash` that takes no arguments and returns a `long`, an `equals` that takes one value of the declaring type, and a `toString` that takes no arguments each write it as an `override`.
A struct has no base class, so on a struct the marker names either one of those three behaviors or a method an interface the struct implements declares.

Every class value has a text form, rendered from the value's runtime type wherever `.toString()` is called or an interpolation hole holds the value.
A class with no `toString` override anywhere in its chain returns its runtime type's name, so a subclass prints its own name even through a base-class-typed or interface-typed reference.
A generic class renders with its arguments the way diagnostics spell them, such as `Box<int>`.
A class replaces that default by declaring `override toString() -> string` with a body, under the shape rule below: no parameters, a `string` result, and never `throws`.
The replacement dispatches from the runtime type, so a subclass's `toString` wins through a base-class-typed or interface-typed reference, and a subclass may override an ancestor's `toString` like any other method.
A `toString` override cannot be `abstract`: every class already has a text form, its type's name, so there is no text form left unwritten.
The name `toString` is reserved for the text form wherever a declaration under it takes no parameters, on a struct and on a class alike: such a declaration must read `override toString() -> string`, and every other shape a parameterless one could have is an error naming that form.
So a parameterless `toString` that returns something other than a `string`, one that declares `throws`, and one that leaves the marker off are each refused, the last because it would replace the text form the type already has while nothing in its declaration shows that.
A method named `toString` that takes parameters is an ordinary method of either kind, so calls reach it by name, and interpolation holes and the built-in text form do not use it.
Like a `toString` on a struct, one written `override` follows its class's visibility when unmarked and may not be marked less visible than the class itself.

Inside a method, a constructor, or a destructor, a struct's or a class's own fields and methods are reached through `this`, as in `this.width` and `this.area()`, because a bare name there is a local, a parameter, or a top-level declaration and never a member.
A field's default value is the one place a member is named on its own: a default may read the fields declared before it in the same class, as `string name = "{width}x{height}";` does.
A default reads those fields and assigns none of them, so `int b = (a = 5);` is an error where `a` is a sibling field.
A class computes the defaults of its own fields in declaration order when its constructor starts, after the arguments of `new` or `super(...)` are evaluated and before the body runs.
An assignment in the constructor body then replaces a field's default, except that a `const` field with a default can never be assigned.
A struct that declares a constructor computes them before that constructor runs.
In either a default may read an earlier field only when that field has a default of its own, or when its type has a default value, such as a number or a nullable type, and it is not `const`.
A struct literal assigns the fields it lists before it computes the defaults of the others, so in a struct that declares no constructor a default may read any earlier field.
`this` is not available in a default, since the object is still being built.

`super.method(...)` calls the base class's implementation, bypassing any override. `protected` members (see above) are reachable from subclasses.

A subclass constructor builds its object in two phases, divided by the `super(...)` statement that constructs the base class.
In the first phase it assigns its own fields, while the fields the base class declares have no value yet.
`super(...)` then runs the constructor of the base class, which builds the base part the same way.
Once `super(...)` returns, every field has a value, so the rest of the constructor may call any method on `this`, overrides included, and pass or store `this`.
An override the base constructor calls sees the subclass's fields set, because they are assigned before the base constructor runs.

```ens
public class Named {
    protected string name;
    protected constructor(this.name) {
        print(this.describe());     // runs the override of the class being built
    }
    protected describe() -> string { return this.name; }
}

public class Greeting extends Named {
    string greeting;
    constructor(string name, string greeting) {
        this.greeting = greeting;   // the first phase assigns the subclass's fields
        super(name);                // the base class is constructed here
        print(this.describe());     // the second phase may use 'this' in any way
    }
    protected override describe() -> string { return "{this.greeting}, {this.name}"; }
}
```

An own field needs a value when it has no default and its type has no default value, or when it is `const` and has no default.
Every own field that needs a value is assigned on every path before `super(...)`, and a `this.field` parameter counts.
A constructor that writes `super(...)` calls it exactly once on every path that finishes it, as a statement of its own.
It may call it in each branch of an `if` or a `switch`, but not inside a loop, a lambda or a `catch` clause, and a `return` before it is an error.
Before `super(...)` returns, `this` may only be used to assign the subclass's own fields, to read those that have values, and to test its type with `is`, and a field the base class declares can neither be read nor assigned.
A `this.field` parameter that names a field of the base class assigns it once the base class is built.
A constructor that does not write `super(...)` constructs its base class with the base's zero-argument constructor where it finishes: at the end of its body, at a `return`, and at the end of a `catch` clause.
Its whole body is then its first phase, and the base class must have a zero-argument constructor this class can reach.
A class that declares no constructor gets one that takes no arguments and does the same.

Class fields may declare default values just like struct fields.
A class that declares no constructor computes its defaults where a subclass's constructor builds it, with `super(...)` or where that constructor finishes.
A field default and a parameter default may name the enclosing declaration's type parameters, and each runs with the type arguments written where the instance was created or the call was made.
So inside `class Box<Tag>` a field may read `List<Tag> items = new List<Tag>();`, and a method may declare `takes(List<Tag> more = new List<Tag>())`.

A field whose type has no default value, such as a class, a string, an array, or a struct without one, and that is not made nullable, must be definitely assigned on every path through each constructor.
A `this.field` parameter, a declared field default, and a `this.field = ...` assignment in the body all count, and assigning the field in every branch of an `if` or `switch` satisfies the rule exactly as a single unconditional assignment does.
A constructor that leaves such a field unassigned on some path, for example by returning early before it is set, is a compile error, and a class that has such a field but declares no constructor at all is rejected the same way.
Such a field may be read through `this` only where every path reaching the read has assigned it.
A compound assignment such as `+=`, a write to a member of the field, and a call of the function value the field holds all read it.
In a `catch` clause, a field counts as assigned only when the body assigns it before every point where the body can throw an exception the clause catches.
The fields a base class declares have their values once `super(...)` returns, so neither the first phase nor a `catch` clause that catches what the first phase throws may read them.
Until every field that needs a value has one, inherited fields included, a constructor may use `this` only to read and write its fields and to test its type with `is`.
Before that point, calling a method on `this` or `super`, passing `this` to a call, storing it in a variable, a field or an array, and creating a lambda that uses `this` are errors, because the code they reach may read a field that has no value yet.
A `switch` over `this` may test its type with `is` arms, but an arm that names the value, as `is Square square ->` does, stores `this` in that name.
Writing `this` in an interpolation hole calls `toString` on it, and comparing it with `==` or `!=` calls `equals`, so both are errors there as well, unless the type is a `final` class and no class in its chain declares that method.
In a constructor that writes `super(...)`, a `catch` clause that catches an exception thrown before `super(...)` returns, by the first phase or by the arguments of `super(...)`, runs before the base class is constructed.
Such a clause may not reach its end or a `return`: every path through it ends by calling `panic` or another `noreturn` function, or loops forever.

A field may also be `const`, in a struct as in a class: a const field is given its value when the object is constructed and never changes afterwards.
It is assigned by a constructor of the type that declares it, through a `this.field = ...` statement or a `this.field` shorthand parameter, and a struct's const field is equally set by an aggregate literal naming it.
Construction gives it exactly one value: either its default value or one assignment, never both.
A second assignment on any path through a constructor is an error, and so is an assignment inside a loop, which could run more than once.
A const field without a default value must be assigned on every path through every constructor, whatever its type, a class that declares one but no constructor is rejected, and every aggregate literal must name it.
A nullable const field is no exception: it does not start as null, so `const string? label;` needs a value like any other const field.
A const field with a default value has that default as its one value, so no constructor may assign it, no `this.field` parameter may name it, and no aggregate literal may list it.
Until every field that needs a value has one, `this` cannot leave the constructor, so no code outside it can see a const field before it has its value, and none ever sees it change.
Everything else is an error: assigning from a method, a destructor, or a free function, assigning through any reference other than `this`, assigning an inherited const field from a subclass constructor, and `++`, `--`, or a compound assignment anywhere.
Writing a field of a const field of a struct type is an error too, because a struct's fields are part of its value.
That holds at any depth, for `=`, a compound assignment, `++`, and `--`, and in a constructor as anywhere else, so a constructor gives such a field its whole value in its one assignment.
Calling a `mutating` method on such a field, or on a struct field of it, is an error for the same reason.
A const field may be nullable, and it cannot also be `weak`, because a weak field resets to null when its target is destroyed while a const field never changes.

```ens
public class Download {
    const int retries = 3;      // the default is the one value of 'retries'
    const string? mirror;       // no default, so every constructor assigns it, even to null

    public constructor(string? mirror) {
        this.mirror = mirror;
        // this.retries = 5;    // error: 'retries' already has its default value
    }
}
```

A field carries visibility modifiers, `const`, `static` (which requires `const`), `lazy` (which requires `const` too), and in a class also `weak`; no other modifier applies to one.
A method carries visibility, the overriding markers (`abstract`, `override`, `final`), `noreturn`, and `static`, and a struct or interface method also `mutating`, while a constructor carries visibility alone, and a function declared at the top level carries visibility and `noreturn` alone, because nothing inherits it.
A destructor carries no modifier at all, because no code ever calls it.
`sealed` belongs to a class, so it does not apply to a field or a callable; `const` belongs to a field or a variable inside a function, so it does not apply to a callable.
Writing a modifier where it does not belong is an error that names where it does.

A class or a struct may declare `static` members: methods and `const` fields that belong to the type itself rather than to any instance.
A static member is reached only through the declaring type's name, never through an instance: `List.withCapacity(8)` is legal, and `myList.withCapacity(8)` is an error naming the spelling to use.
Statics are not inherited: a static declared by a base class is reached through the base class's own name, and naming it through a subclass is an error that names the declaring class.
The rule is uniform, so even inside the declaring type a static is called through the type name, and `this` cannot appear inside a static method, because a static has no instance.
A static method may be `throws`, which is what makes static factories useful where a constructor cannot throw, and it may be `noreturn`.
A static and an instance member cannot share a name, and the instance members of a type include the ones it inherits, so a static may not take the name of a base class's field or method either.
`abstract`, `override`, and `final` do not apply to a static, which never takes part in dispatch.
Static methods may overload each other under the ordinary overload rules.
Interfaces, enums, and external types cannot declare statics; the error names where statics belong.
A static cannot be reached through a type parameter: `T.create()` is an error, because `T` stands for a different type in every instantiation.

A `static const` field is a type-level constant with a mandatory initializer: `static const string separator = "/";`.
There are no mutable static fields, so `static` on a field requires `const`.
The initializer is a compile-time constant: literals, unary `+` and `-`, arithmetic with `+`, `-`, `*`, `/`, and `%`, string concatenation with `+`, and reads of other static consts through their type name.
A cycle between static const initializers is an error, and so are function calls, `new`, aggregate literals, and array literals inside one.
A static const's type is therefore a primitive, `char`, `bool`, or `string`, and on a generic type neither it nor its initializer can mention the type's parameters, so every instantiation shares one value.
Reads go through the type name, as in `Path.separator`, and each read compiles to the constant's value; assigning to a static const, or applying `++` or `--` to one, is an error.

A `lazy const` field is a type-level value with a written type and a mandatory initializer: `export lazy const FrozenMap<string, string> defaults = loadDefaults();`.
The initializer runs on the first read and the value is kept for the rest of the program, so unlike a static const's it may call functions, allocate, and build aggregates.
It may not throw, which is what keeps a read from needing `try`.
A lazy const is read through the type name, as in `Settings.defaults`, and it is type-level already, so writing `static` beside `lazy` is an error.
Every task in a program can read a lazy const by name, so its type must be shareable, as the section on tasks defines; `lazy const List<string> names` is refused naming the type, and a `FrozenList<string>` is what to write there.
The value's destructor never runs, because the program holds the value to the end.
On a generic type a lazy const's type and its initializer cannot mention the type's parameters, so every instantiation shares one value.
Assigning to a lazy const, or applying `++` or `--` to one, is an error.
So is writing a field of a struct-typed lazy const, at any depth and in any of those forms, because a struct's fields are part of its value.
So is calling a `mutating` method on one.
A cycle between two lazy const initializers is a compile error where the initializers read each other directly; one that closes through a function call stops the program on the read that closes it, naming the value.

A static of a generic type takes the type's arguments in one of three ways.
They may be written on the type name: `List<int>.withCapacity(8)`.
They may be inferred from the call's own arguments, the way a generic function call infers them: `List.of(items)` takes `T` from the element type of `items`.
They may be target-typed from the surrounding context, using the same context set as aggregate literals (the declared type of a variable, a parameter, a return type, an assignment target, or an array element): `List<int> xs = List.withCapacity(8);`.
Explicit arguments on the type name always win, and a bare call that none of the three sources completes is an error whose fix is to write the arguments on the type name.
Type arguments are never written on the method name: `List.withCapacity<int>(8)` is rejected.

A class may declare a `destructor`, introduced by the `destructor` keyword, to run cleanup when an instance is destroyed.
A destructor takes no parameters, has no return type, and cannot be `throws`; a class declares at most one, and it cannot be called explicitly.
When the last reference to an object is released its destructor runs, followed by each inherited destructor from the most derived class up to the base, and then the object's fields are released.
A local releases its reference when the block it is declared in ends, or earlier when an assignment replaces its value, and the same holds at every optimization level.
Returning a local hands its reference to the caller instead.
Copying a local into another local, a field, an array element or an argument leaves the local holding its own reference, so the copy never ends the object's life before the local releases it.
A value no local holds, such as an object made inside an argument, is released by the end of the statement it is made in.
When an argument of a `new` expression throws, the constructor never runs, so no destructor runs for that object either, and its fields hold no values yet.
An assignment stores the new value before it releases the old one, so a destructor that release runs finds the new value in the field or element, and a value that destructor writes there is the one kept.
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
An interface cannot declare fields, constructors, or method bodies, and it cannot use `implements`.
A throwing interface method must list its thrown types explicitly (`load(string path) -> string throws IOError;`), the same rule as abstract methods.

```ens
interface Speaker {
    speak() -> string;
}

interface Source<T> {
    take() -> T;
}
```

An interface may extend one other interface with an `extends` clause: `interface Collection<T> extends Iterable<T> { ... }`.
The extending interface inherits every requirement of the interface it extends, and chains may be any depth.
Redeclaring an inherited requirement (the same name and parameter types) is an error naming the interface that already declares it; an overload with different parameters is a new requirement like any other.
The `extends` target must be an interface: naming a class, struct, enum, primitive, or type parameter is an error, and so is an interface extending itself, directly or through a chain.
An interface cannot extend a less-visible interface, under the same rule that stops a class from extending a less-visible base class.

A class names the interfaces it implements in an `implements` clause after the optional `extends`, separated by commas: `class Dog extends Animal implements Speaker, Source<string> { ... }`.
The class must provide every method of every listed interface with the exact signature, including the methods of every interface those extend, either declared in the class or inherited from a base class; a missing or mismatched method is a compile error naming the interface that declares it and the signature.
A method declared in the implementing class that provides an interface method must be marked `override`, exactly like the override of an abstract base method; a satisfying method inherited from a base class needs no marker.
An abstract class may declare an interface method `abstract override` and leave the body to its concrete subclasses.
A method satisfying a throwing interface method may throw the declared types or their subclasses, never others; satisfying a non-throwing interface method means not throwing at all.
A struct names the interfaces it implements the same way, and is held to the same requirements and the same `override` marker; what a struct's conformance does and does not buy is described in the struct section.

An interface name is a reference type usable wherever a class type is: variables, parameters, returns, fields, generic type arguments, `I?`, and arrays under the same element rules as classes.
A value of an implementing class converts implicitly to each interface it implements and to every interface those extend (and to `I?`); there are no implicit conversions between unrelated interfaces.
A value of an extending interface's type converts implicitly to any interface it extends, directly or through the chain, and `is` and `as?` between interface types follow the same relation, determined by the value's runtime type.
A call through an interface-typed value dispatches on the value's runtime type, so a subclass override runs even when the call happens through the interface.
`==` and `!=` on interface-typed values compare identity, exactly like class references.
An interface cannot be instantiated with `new`, and a `weak` field cannot have an interface type; weak references stay class-only.

```ens
Speaker s = new Dog();     // implicit conversion to the interface
print(s.speak());          // runs the implementing class's method
Speaker? quiet = null;     // nullable interface reference
```

---

Classes, structs, and functions may be generic: they declare type parameters in angle brackets and work uniformly over any type argument. A type parameter can be used as a field type, a parameter or return type, a local type, and as the element type of an array.
A type-parameter list names at least one type parameter, so an empty `<>` is an error.
A method does not declare type parameters of its own; a type-parameter list on a member constrains the enclosing type's parameters instead, as described under conditional members below.
A type parameter takes its name for the whole of the declaration that introduces it, so that name is the parameter in a bound, in a member's signature, in a field or parameter default, and in a body alike.
A class, struct, interface, enum, function, or module of that name declared or imported outside is unreachable under that name inside, while a parameter or a local of that name declared inside takes the name back for the values written after it.
A type parameter is a type and never a value, so reading `T` as a value, calling `T()`, and writing `new T()` are each an error.

```ens
class List<T> {
    T[] items;
    long count;

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

For a generic function, the type arguments can be passed explicitly or inferred from the call.
A type argument is inferred wherever a parameter type mentions it, so `T` is inferred from a parameter of type `T`, `T[]`, `T?`, `List<T>`, `(T) -> bool`, or any nesting of these.
The match is structural, so a `Map<K, List<V>>` parameter binds both `K` and `V` from a `Map<string, List<int>>` argument.
It also looks through the interfaces a class implements and the classes it extends, so a class implementing `Iterable<int>` binds `T` for an `Iterable<T>` parameter, and so does a value of an interface extending `Iterable<int>`.
The first argument that mentions a type parameter binds it, and a later argument that disagrees is an error naming that binding and the argument it came from.
An argument with no type of its own, such as an array literal or a `?:` holding a lambda, binds a type parameter through those of its values that have a type of their own: passing `[twice, (x) -> x + 1]` for a `T[]` parameter binds `T` to the type of `twice`, and the lambda then takes that type.
Explicit type arguments always win over what the call would infer.
A lambda binds the type parameters its target's return type and throws list mention, once its parameter types are known from the target or from an earlier argument: `attempt(() -> compute())` binds `V` to what the body returns and `E` to what it throws, so a type parameter only a lambda's result mentions needs no explicit argument.
A type argument no argument mentions, such as one that appears only in the return type, must be passed explicitly.
`void` means no value, and it is a type argument like any other, written as in `Holder<void>` or bound by a call.
A function or a lambda that returns nothing, passed for a `() -> T` parameter, binds `T` to `void` unless another argument gives `T` a type, which then decides it, so `after(callback, 1)` against `after<T>(() -> T first, T second)` binds `T` to `int` and refuses `callback`.
An argument that is itself no value, such as a call to a function that returns nothing, is still refused wherever a value is passed.
A value of type `void` takes no storage: a local, a field, a parameter or a capture of that type holds nothing, a struct literal leaves such a field out, and a function returning `T` returns nothing, so `Task<() -> void>` carries no value and `Channel<void>` carries only that something was sent.
A `T?` over `void` is present or absent and holds nothing else.
A field of type `void` takes no part in a struct's `==`, its hash, or its JSON form, while a value of type `void` has no text form, so writing one in an interpolation is refused.
A parameter that a `void` type argument makes `void` takes no argument, so `same<void>(1)` is refused.
An array of `void` would have nothing to hold, so a generic that makes one, as `List<T>` and `Map<K, V>` do for their storage, refuses `void` at the instantiation under the rule for judgments deferred to type arguments, naming the line that makes the array; `List<void>` is therefore an error, and so is `void[]` anywhere.

```ens
firstOf<T>(List<T> items) -> T {
    return items.get(0);
}

count<T>(Iterable<T> items) -> long {
    long total = 0;
    for (let item in items) {
        total = total + 1;
    }
    return total;
}

swap<int>(1, 2);          // explicit
swap(1, 2);               // T inferred as int
firstOf(names);           // T inferred as string from the List<string>
count(new Range(1, 3));   // T inferred as int because Range implements Iterable<int>
```

A type parameter may declare bounds with `T: Base + Comparable<T>`, joined by `+`: at most one bound may be a class (conventionally written first) and every other bound must be an interface, and listing the same bound twice is an error.
A bound may not name a `final` class: no type except that class itself could satisfy it, so write the class type directly instead.
Every type argument must satisfy all bounds, being the class or a subclass of it and implementing each interface; a violation is a compile error naming the failing bound.
A judgment a generic body defers to its type arguments, such as an array element that needs a default value or an interpolated value that needs a text form, is reported where those arguments were written and names the instantiation written there.
The line inside the generic that could not be judged without them is reported beside it as a note naming that generic.
Inside a member of a generic type, `this` has the type at its own type parameters, such as `Pair<T>`, so a judgment on `this` is made for each instantiation as well.
An interface bound is also satisfied through interface extension: a class implementing an interface that extends the bound satisfies it, and so does the extending interface itself as a type argument.
The body may use the members of every bound on a value of that parameter.
The `Animation<S: Shape + Comparable<S>>` example above uses exactly this form.

```ens
class Drawer<T: Shape> {
    T shape;
    constructor(T s) { this.shape = s; }
    area() -> int { return this.shape.area(); }
}

summarize<S: Shape + Comparable<S>>(S value) -> string {
    return value.describe() + " / " + value.compareTo(value);
}
```

A generic class may extend another generic class by naming the base with full type arguments; the arguments may use the subclass's own type parameters. Overrides and virtual dispatch work as with ordinary inheritance, per specialization.

```ens
abstract class Source<T> {
    abstract read() -> T;
}

class Constant<T> extends Source<T> {
    T value;
    constructor(this.value);
    override read() -> T { return this.value; }
}

Source<int> source = new Constant<int>(5);
source.read();
```

A member of a generic class or struct, whether a method, a static, or a constructor, may constrain the enclosing type's parameters by restating them in its own type-parameter list with a bound.
Such a member is conditional: it exists only on the instantiations whose type arguments satisfy the constraint, and it costs nothing anywhere else.
A name in the list must match one of the enclosing type's parameters and must carry a bound.
Restating a parameter without a bound is an error, which is what makes a rename of the type's parameter detectable, and a name that matches no parameter is an error too, because a member cannot introduce type parameters of its own.
The constraint's bounds follow the same rules as declared bounds, and a bound may name the constrained parameter itself, as `Comparable<T>` does below.

```ens
interface Comparable<T> {
    compareTo(T other) -> int;
}

class List<T> {
    push(T value) { /* exists on every List<T> */ }

    // exists only on instantiations whose T implements Comparable<T>
    sort<T: Comparable<T>>() {
        // inside the body, T carries the extra bound, so T values have compareTo
    }
}
```

Calling a conditional member, or constructing through a conditional constructor, on an instantiation that does not satisfy the constraint is an error naming the member, the parameter, the unmet bound, and the failing type argument.
Overload resolution never sees a conditional member where its constraint fails; where it holds, the member participates like any other, so a conditional constructor can sit beside an unconditional overload and each instantiation gets the constructors that exist for it.
Inside the member's body the constrained parameter carries the extra bound, which also lets the body reach other members conditional on the same bound.

A conditional member may not be `override` or `abstract`, may not be overridden or hidden by a subclass, and cannot satisfy an interface requirement, because all of those require a member that exists on every instantiation.
An interface method cannot have a type-parameter list for the same reason, and a destructor cannot have one because it takes no parameters of any kind.
A constraint cannot relate two different parameters, so a bound like `K: Comparable<V>` is not expressible.

---

A function value is a value that can be called.
Its type is written as a parameter list, an arrow, and a return type: `(int, int) -> int` takes two integers and returns one, and `() -> string` takes nothing.
The return type may be `void`, which means the call produces no value; a parameter type may not be, because there is no value to pass.

A `?` or `[]` suffix written after a function type belongs to the return type, so `(int) -> bool?` is a function returning `bool?`.
A nullable or arrayed function type is therefore written in parentheses: `((int) -> bool)?` and `((int) -> bool)[]`.
A `throws` list parses the same way, which is the other reason parentheses are allowed here.
A function type is the only type parentheses are allowed around, and nothing else needs them: a function type returning a function type groups right to left, so `(int) -> (int) -> int` returns `(int) -> int`, and a function type is written directly as another's parameter type, as in `((int) -> bool, int) -> int`.
Parentheses around any other type, as in `(int) x = 1;`, are an error naming the plain spelling, so every type keeps one written form.

Everywhere else a function type is an ordinary type.
It can be a field's type, a parameter's type, a local's type, a return type, a generic type argument such as `List<(int) -> bool>`, and an array's element type.

A function value is written as a lambda: the parameters in parentheses, an arrow, and the body.
The parentheses are always required, even around a single parameter, so `(a) -> a * 2` is the form and `a -> a * 2` is an error.
The body is either one expression, whose value the call returns, or a block written `(a, b) -> { ... }`, which returns the way any function body does.

A lambda takes its shape from the type it is written against, so it always needs one: a local or field with a declared function type, a parameter, a return type, an assignment target, an array element, or an aggregate literal's field.
A lambda with nothing to take its shape from, such as `let f = (a) -> a;`, is an error, exactly as an aggregate literal with no context is.
A lambda written as a branch of `?:` or an arm of a switch takes its shape from the type the whole `?:` or switch is written against, and where there is none, from the type of the other branches (see the switch section).
The parameter types come from that type, and the number of parameters must match it.
A parameter's type may also be written, as in `(int a, int b) -> a - b`, and then it must be exactly the type the target passes there; one lambda writes every parameter's type or none of them.

A lambda copies the values it reads from around it, and only those.
Reading a local, a parameter, or `this` inside the body captures it: the closure keeps its own copy from the moment it is created, and a class reference it captured stays alive for as long as the closure does.
A lambda written against a `shared` function type captures only shareable values, as the section on tasks defines, and in such a lambda a read of a `const` field of `this` whose type is shareable captures that field's value rather than `this`, so a lambda inside any class may read the object's `const` fields and still be `shared`.
A local a lambda captures must already be assigned where the lambda is created.
A narrowing that holds where a lambda is created holds inside its body for what the lambda captures, because a capture is a copy that nothing can write.
So `check(string? held) -> () -> bool { if (held == null) { return () -> false; } return () -> take(held); }` compiles, and `held` is a `string` inside the second lambda.
The narrowing is the one where the lambda is created: assigning the local afterwards changes neither the copy nor its narrowing.
The same holds for a narrowing of `this`, of a `const` field reached through what the lambda captures, and of a `lazy const` read through its type name, since nothing can write any of them.
A `const` field of a class object is read from the object each time the lambda runs, and it already has its one value wherever code can reach the object, so a narrowing of it holds there too.
A check inside the body narrows a captured local or parameter wherever the check holds, as it narrows the body's own locals.
Assigning to a captured local inside the lambda, or passing it as `out`, is an error, because the write would land on the copy and leave the local outside it unchanged; return the value the lambda computes instead, or keep the value in an object the lambda can reach.
A field of a captured struct is part of that copy, so writing `point.x = 1;` inside the lambda is the same error.
The lambda's own parameters and locals are ordinary storage and may be written freely.

`this` is captured like any other reference, so a lambda inside a class's method may read the object's fields, write them, and call its methods.
A struct's method cannot hand its `this` to a lambda, because a struct is a value: the lambda would capture a copy, and a write through that copy would not reach the struct the method runs over.

A function type raises nothing unless it declares what it raises, with a `throws` list written after the return type.
The function type carrying the list is always written in parentheses, as in `(() -> void throws ParseError)`, so the two readings a trailing `throws` could have always look different and there is no precedence rule to know.
A declaration returning a function type puts the parentheses where the list belongs: `handler() -> ((int) -> int throws ParseError)` returns a function that raises, `handler() -> ((int) -> int) throws ParseError` raises itself, and the bare spelling with no parentheses is an error naming both forms.
The list is required rather than optional, for the same reason an abstract method's is: a function type has no body to infer one from.

A lambda's body is held to its target's list, so a lambda written against `(() -> void throws ParseError)` may raise a `ParseError` and nothing else.
A lambda whose body can raise something its target does not list is refused, and so is one written against a target with no list at all; the fix in either case is to add the type to the target's list, or to handle the failure inside the lambda by moving the throwing work into a named function whose own `catch` clause turns it into a value.

A call through a function value that declares a list needs `try`, and the caller handles the declared types, exactly as a call to a declaration with that list would.

Assignability between function types is exact, and the throws list is part of the type.
A `(long, long) -> long` is not accepted where a `(int, int) -> int` is expected, and neither is the reverse, whatever the parameter and return types would allow on their own.
A `() -> void` and a `(() -> void throws ParseError)` are likewise two different types, in either direction.

A function value is called with ordinary call syntax on the value itself, wherever that value comes from: a local, a field, an array element, or the result of another call.
A function type has no parameter names, so the arguments are positional and named arguments are an error, and the call passes exactly as many arguments as the type declares.
A nullable function value is checked for null before it is called, exactly like any other nullable reference.

A function value has no identity and no text form.
Comparing two function values with `==` or `!=` is an error, as are `hash()`, `toString()`, and putting one in an interpolation hole.
Testing a nullable function value against `null` is a presence check rather than a comparison, and stays allowed.

```ens
sortWith(long[] values, (long, long) -> int order) { /* ... */ }

orElse(string? text, () -> string make) -> string {
    return text != null ? text : make();
}

class Counter {
    long total;

    constructor() { this.total = 0; }

    public adder() -> (long) -> void {
        return (by) -> this.total = this.total + by;   // captures 'this'
    }
}

report(long[] values, long threshold) {
    sortWith(values, (a, b) -> (a - b) as int);        // parameter types from the target
    let label = orElse(null, () -> "none");
    ((long) -> bool)? accept = (v) -> v > threshold;   // captures 'threshold' by value
    if (accept != null) {
        accept(values[0]);
    }
}
```

---

Imports are based on paths, and each form brings one kind of name into scope.
A module import qualifies the module's functions with the module's name, and a name import brings one of its types into scope under that name.
Imports are file-local.

```ens
import engine.renderer;
```

This allows usage like `renderer.configure();`

Or:

```ens
import Renderer from engine.renderer;
```

Allows `new Renderer();`

In both cases, the path is `/src/engine/renderer.ens` and the file contains a public `Renderer` class.

Either form may rename what it brings into scope with `as`, written right after the name it replaces.

```ens
import engine.renderer as gfx;
import Renderer as Canvas from engine.renderer;
```

The first binds the module to `gfx`, so its functions are called as `gfx.configure()`, and that import binds nothing under `renderer`.
The second binds the type to `Canvas`, and that import binds nothing under `Renderer`.
An alias is an ordinary identifier, so it may not be a keyword, and it collides with the file's other imports and declarations exactly as the name it replaces would.
Aliasing is how one file uses two modules that share a last segment, or two types that share a name: `import ErrorKind as IoErrorKind from @std.io.streams;` beside `import ErrorKind as FileErrorKind from @std.fs.error;` gives the file both.
An alias may be written whether or not a conflict exists, an alias equal to the name it replaces changes nothing, and one file may alias the same module more than once.
A function cannot be imported by name with an alias any more than without one.

A diagnostic names a type the way the file it is reported in can name it, which is the name that file declares or imports it under.
A type the file has no name for is named on its own.
Where the file gives that name another meaning, or another module declares a type of the same name, the module it comes from follows the quoted name, as in `'Ruler' (from engine.sizing)`.
Where the quote also names another declared type, such as a type argument, the type's name comes before its module, as in `'Box<Ruler>' (Ruler from engine.sizing)`.
Where one name in a quote stands for two types, the module note identifies which by position, as in `'Pair<Ruler, Ruler>' (the second Ruler from engine.sizing)`.
The module is written as the file would import it, so a type of the standard library is written as in `'Path' (from @std.fs)`.
A quote with no module after it can be written in that file exactly as the message spells it, after adding any import the message suggests.
Where a module follows the quote, the file first needs an import that gives the type a name, as in `import Ruler as SizingRuler from engine.sizing;`.

A file and a folder with the same name may sit side by side: `io.ens` next to an `io/` folder makes `import io;` resolve to the file, while `import io.streams;` resolves to `streams.ens` inside the folder.

Source files are UTF-8 text.
A byte order mark at the very start of a file is accepted and skipped; the same bytes anywhere else in the file are an error.

A module's private declarations never leave its file; `public` declarations are visible to every module in the same package, and `export` declarations also to the packages that consume it.
A type is reached only by the name the file binds to it, its own declaration or an import, so a module name never qualifies a type: after `import engine.renderer;` the spelling `renderer.Renderer` is an error that names the import to write.
Free functions are the other way round, always called through their module name and never imported by name: write `import engine.renderer;` then call `renderer.configure()`.
Importing a function by name (`import configure from engine.renderer;`) is an error.
The implicitly imported modules described below are the one exception: no import is written for them at all, so their exported functions are called unqualified.

Two modules may import each other: circular imports are allowed, and declarations resolve across the cycle like any other import.

A short list of standard-library modules is imported implicitly: `@std.core` and `@std.io.print`.
Every exported name of such a module is in scope in every module with no import written: `@std.core`'s types `Error`, `StackFrame`, `Comparable`, and `Copyable` are used by name, and both modules' exported functions are called unqualified.
So `print(message)` and `eprint(message)`, which write a line to standard output and to standard error, come from the standard library rather than from the language, and no program imports anything to print a line.
A declaration of your own with one of those names takes precedence inside the module that declares it.
That precedence goes by name and not by signature: a `describe(int)` of your own hides every implicitly imported `describe`, so a call that does not fit yours is an error rather than a call to theirs, and the hidden one is reached by importing its module and qualifying the call.
Every other module of the standard library has to be imported.

A module may also declare a function with the same name and parameter types as one the language itself provides, such as `panic`.
Every call to that name in the module that writes the declaration goes to it, and every other module keeps the function the language provides.
Such a declaration is an ordinary function and takes nothing from the one it stands in for: a `panic` of your own never returns only if you write `noreturn` on it.
A declaration whose parameter types differ is an ordinary overload alongside the provided function, exactly as two declarations of your own overload each other.
A generic declaration of that name is rejected either way, because a generic function cannot be overloaded and replacing the provided function needs its parameter list.

A program's entry point is the top-level function `main` in its **main module**: `src/main.ens` for a package or a folder of sources, or the compiled file itself for a single-file program.
No other module may define a top-level `main`; the compiler rejects one wherever it is loaded from, including through an import of another package's main module.

`main` is declared either as `main()` or as `main() -> int`, and it may add `throws` or `noreturn` like any other function.
The `int` it returns becomes the process exit code, a `main()` that returns nothing exits with 0, and an exception that escapes `main` is reported on stderr and exits with 1.
Any other return type is an error, and so are parameters and type parameters: nothing passes arguments to `main`, and a program reads its command line through `environment.arguments()` from `@std.environment`, which returns the arguments without the program's own name.

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

A package is named by its `package` declaration, and the name `std` is reserved for the standard library, so no other manifest may declare it.
A package's sources live in its `src/` folder, with its tests in a sibling `tests/` folder.
A package whose `src/main.ens` defines `main()` is an application and builds to an executable; a package without one is a library.
Every package declares `ens`, the language version it is written for as major.minor; `version`, the package's own version in dotted numerals, is optional.
The shape of both is checked.
The `ens` version is the oldest language version the package can be built with, and which toolchain builds it is described with the `ens` command below.
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
Every member must declare the same `ens` version, because one workspace is built by one toolchain; a disagreement is an error naming the members and the versions they declare.
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
Workspace members and overridden packages are never pinned in it: a member is used exactly as it is checked out and an override only names where a package comes from, so neither has a version, a source, or a commit to record.
A workspace member that binds a prebuilt artifact is still listed under its own name, so that artifact and its hash are recorded alongside the fetched packages' rather than nowhere.
Builds keep the lock current: when the manifests' requirements change, the build re-resolves what changed, rewrites the lock, and prints a summary of the difference.
`--locked`, accepted by build, check, run, and test, turns any needed lock change into an error, which suits a CI build.
`--offline` forbids all network use: anything not already in the local cache fails the build by name.

`native` declarations name the native libraries the package's `external` blocks bind to (see the section on native calls), and the linker links each library they name.
`native libc system;` declares a library the platform links by default, so nothing extra is passed to the linker.
For an arm64 macOS target, `ens` links that library from a list it carries of what macOS's system library provides, so the build needs no Xcode and no macOS SDK on the machine.
A program that links no other native library is then the same file whichever machine built it, and it records the version of the SDK the list was read from as the SDK it was built against.
Any other native library a macOS program links is looked for in the folders `LIBRARY_PATH` names and in the SDK that `SDKROOT` or `xcrun` names.
For a Linux target, `ens` links glibc's C library, maths library and loader, and libgcc_s, from lists it carries of what each provides, and starts the program with code of its own, so the build needs no C compiler and no Linux libraries on the machine, and any machine can build for Linux.
A Linux program built this way runs on any system with glibc 2.34 or later.
`pthread`, `dl`, `rt`, `util` and `anl`, which glibc 2.34 folded into its C library, and `c`, `m` and `gcc_s` are already among those libraries, so naming one for Linux adds nothing to the link.
Any other native library a Linux program links is looked for in the folders `LIBRARY_PATH` names and, when the program is built for the machine building it, in the folders that machine's distribution keeps its libraries in.
For a Windows target, a program that names no native library beyond the system's own links `kernel32`, `shell32` and the C runtime Windows ships with the system from lists `ens` carries of what each provides, and starts with code of its own, so the build needs no Visual Studio and no Windows SDK on the machine, and any machine can build for Windows.
A Windows program that links any other native library links the way that library was built, with Visual Studio's static C runtime from the folders `LIB` names, because that library was compiled against it.
`native zlib;` links the library under its conventional platform name.
The block form spells out base names per platform (`windows`, `linux`, `macos`), several per platform when needed.
A platform may instead bind a prebuilt artifact: `windows artifact "https://example.com/z.lib" hash "sha256:...";` downloads the file once into the cache, verifies it against the declared sha256 hash (a mismatch fails the build), and passes it to the linker.
An artifact is a single library file for its platform.
Artifact bindings and their hashes are recorded in `ens.lock`, so the exact native code a build links is reviewable in one place.
When two packages in one build declare the same native library, the declarations must be identical; identical declarations are linked once.
Two declarations of a native library are identical when they have the same form (bare, `system`, or a binding block) and, for a block, name the same platforms with the same bindings in any platform order.
Within one platform the base names must be in the same order, because they are linked in that order.

`embed "../shared";` lists a folder outside the package that its sources may embed files from, written relative to the folder of `ens.package` with `/` between folders.
Only a package whose sources are the user's own may list one: the program, a workspace member or an overridden package.
A fetched package or the standard library embeds only files inside its own folder, because those are the files it was published with.

---

The toolchain is one command, `ens`, with a subcommand for each thing it does.
`ens help` and `ens --help` list the commands, and `ens help <command>` describes one of them along with every option it takes.
`ens --version` prints the same as `ens version`.

- `ens build [path]` compiles a program and writes an executable.
- `ens check [path]` does everything a build does up to code generation and nothing after it: it reports every problem a build finds before code generation, and produces no artifact.
  It also checks the test files `ens test` reads for the same path, which a build leaves out, so it reports a problem in a test, such as a `main()` declared in a test file.
  A fix the check suggests keeps the tests compiling.
- `ens run [path] [-- arguments]` builds a program in a folder of its own, runs it, and removes the folder, so the tree it was pointed at is left as it was found.
  Everything written after `--` is passed to the program exactly as written, and `ens run` exits with the program's exit code.
- `ens test [path]` builds a target's tests together with its sources and runs them, as described in the section on tests.
- `ens override add <package> <folder>`, `ens override remove <package>` and `ens override list` maintain the `ens.overrides` file beside the build root's manifest.
- `ens toolchain list` and `ens toolchain remove <version>` manage the installed toolchains, as described below.
- `ens version` prints which release of Ens it is and the system and processor it runs on, as in `ens 0.13.0-beta windows/x64`.
  A release's major.minor is the language version it implements, which is the version a package's `ens` declaration names, and its patch counts the releases of that language version.

The path is an `.ens` file, a folder of sources, a package folder, or a workspace root.
With no path, the command works on the nearest package in or above the folder it was run in, found the way `git` finds a repository from a subfolder.
At a workspace root every member is built, checked, or tested, each one after the members it depends on; `ens run` needs the workspace to hold exactly one application and lists the members it found when there is more than one.

An executable is named after what was built: a single file's own name, the last segment of a package's name (`acme.tools` builds `tools`), or the folder's name when a folder declares no package.
It is written to the folder the command was run in unless `--output` names a file.
Any folder on the path `--output` names that is not there yet is created.
A package whose main module defines no `main()` is a library: it is compiled and checked just as thoroughly, no executable is written, and `--output` is refused because there is none to write.
`--output` is an error at a workspace root for the same reason: it builds more than one artifact.

Code generation follows what the program reaches, so a function body nothing reaches is not generated.
A module the program reaches nothing in gets no object file of its own.
A build keeps the object files it produced under the build root, in `.ens/<target triple>/O<level>/`.
The build root is the folder holding the manifest that governs the sources, so where the objects go does not move when `--output` names an executable somewhere else; sources that no manifest governs keep theirs beside themselves.
Every target triple and every optimization level has a folder of its own, so an object built for one configuration is never read by a build of another.
Each kept object file's name carries a digest of its bytes, and a build removes the earlier versions of the objects it keeps.
The `.ens` folder carries a `.gitignore` that ignores everything under it, and a build leaves what it put there in place.
`ens run` and `ens test` are the exception: each builds in a folder of its own under the system's temporary directory and removes that folder afterwards, object files included, so neither leaves anything anywhere.
A run that is stopped before it removes its folder leaves the folder behind, and the next run of the same command removes it, because a folder is marked as in use only for as long as the run that made it is running.
A leftover folder the next run cannot remove is reported along with what to do about it, and the run goes on.
A build links its executable in a folder of its own beside it and then moves it into place, so the path `--output` names never holds a half-written executable.
A build that is stopped while linking leaves that folder, named `.ens-link-<number>`, behind, and the next build that writes an executable into the same folder removes it.
A build generates its object files in a folder of its own inside `.ens/<target triple>/O<level>/` and then keeps them there; when a build is stopped before that, the next build that keeps its objects in the same folder removes what it left.

A command reports every problem it found rather than stopping at the first, and a syntax error does not hide the semantic problems around it: the compiler reads on past it and still reports what it can be sure of, so fixing one problem does not uncover a fresh crop of them.
Where a syntax error leaves what a file declares in doubt, though, it is reported on its own, because any report about the meaning of that file would be a consequence of the syntax error rather than a problem in its own right.
A program that reaches code generation has no problems left to report, so a problem found there is the only kind that can arrive on its own after a run that reported others.

Commands use the same three exit codes: `0` when the command did what was asked, `1` when the program or the project had problems, and `2` when the command line itself could not be acted on.
`ens run` adds one case to that: once the program has run, its own exit code is what comes back.

`build`, `check`, `run` and `test` all accept these options:

- `-O0`, `-O1`, `-O2` and `-O3`, written `--optimization-level <level>` in full, choose how hard to optimize.
  The level may be attached to the short form (`-O2`) or written beside it (`-O 2`).
  The default is `-O2`, and `-O0` turns the optimizer off.
- `--target <triple>` builds for a target other than this machine's own, such as `x86_64-unknown-linux-gnu` or `arm64-apple-macosx14.0`.
  The first part of the triple names the processor: `x86_64` or `amd64` for a 64-bit Intel or AMD one, and `arm64` or `aarch64` for a 64-bit ARM one, and a triple naming any other processor is refused.
  The end of the triple names the system: `windows`, `windows-msvc` or `win32` for Windows with the Microsoft C runtime, `linux` or `linux-gnu` for Linux with the GNU C library, and `macosx`, `macos` or `darwin` for macOS.
  A vendor such as `pc`, `unknown` or `apple` comes between the processor and the system, and it may be left out when the system is one word or starts with `linux`, as in `x86_64-win32` or `x86_64-linux-gnu`.
  A macOS system may end in a version: the macOS version after `macosx` or `macos`, as in `macosx14.0`, or the Darwin version after `darwin`, as in `darwin23` for macOS 14.
  The program then runs on that version of macOS and later ones, and on macOS 11 and later when no version is written.
  `windows-msvc` may also end in a version, as in `windows-msvc19.40.0`.
  A triple naming any other system, such as `linux-musl` or `windows-gnu`, or naming none, is refused.
  The executable is the same bytes whichever machine builds it, and a link of the same inputs writes the same bytes every time: the code is laid out in the order of the inputs, and a profile a library carries does not reorder it.
  One built on Windows for Linux or macOS arrives without the permission to run, which Windows does not record, so give it one there with `chmod +x`.
- `--stdlib <folder>` names the folder holding `std/` to compile against; `ENS_STDLIB` does the same thing.
  A folder either one names that is not there, or that does not hold the standard library in its `std/` folder, is refused before anything is compiled.
  Without either, the build uses the standard library `ens` itself carries, the one it was built with, so a single `ens` file needs nothing else on the machine.
  The first build that needs it writes it into the cache, in a folder named after a hash of its files, and every later build reads it from there; the notes of a diagnostic inside the library name those files.
- `--explain-arc` reports what the reference-counting optimizer elided, as described in the section on memory management.
- `--explain-reachability` reports how much of the program is reached from its entry point: one line per module giving how many function bodies it defines and how many of them are reached, then the name of every function it declares that nothing reaches.
  A program with no entry point is a library, and what a library reaches is counted from its `export` declarations instead.
  The account is not about an optimization, so it reports at every level, and `ens check` prints nothing because it stops before code generation.
- `--offline` and `--locked` govern package fetching and `ens.lock`, as described above.
- `--quiet` reports problems and nothing else, and `--verbose` reports each step as it runs; asking for both is an error.
- `--toolchain <version>` chooses which installed version of Ens does the work, as described below.

`ens build` also takes `--output <file>`, naming the executable to write.
`--strip` leaves the symbol table out of the executable: the names of its functions and variables, which a debugger or a profiler reads and the program itself never does, since a stack trace reads a table of its own.
On macOS a stripped executable also exports nothing, the way a Linux or Windows executable already exports nothing.
A Windows executable carries no symbol table to begin with, so `--strip` changes nothing there.
`ens test` also takes `--filter` and `--tests`, described in the section on tests.

`ens` is also a version multiplexer: a command can be handed to another installed toolchain, and that toolchain's exit code becomes this command's own.
Whenever the newest installed toolchain is newer than the running one, it runs every command except `ens toolchain remove`, so the `ens` a machine runs is always the newest one it has.
A build root declaring a version newer than the running toolchain and every installed one has the newest release of that language version downloaded and installed, and that release runs the command; `--offline` refuses the download instead.
In those comparisons versions compare number by number, so `0.10` is newer than `0.9`, and a label after a release's numbers, such as `-beta`, does not change which version it is, so `0.14.0-beta` and `0.14.0` are the same version.
Toolchains live one folder per version under `~/.ens/toolchains`, each folder holding the `ens` program that is that version, and `ENS_TOOLCHAINS` names a different root to look in.
`ens toolchain list` prints the installed toolchains, the newest first, and marks the one running the command, or the one that is the same release when the running `ens` is outside the toolchains folder.
`ens toolchain list --available` prints instead the newest release of each language version that has a program for this machine, the newest first, written the way its folder is named, and marks the ones that are installed and the one that is the running release.
Releases are listed from the GitHub releases API of `AlexDicy/Ens`, and `ENS_RELEASES` names a different list, such as a mirror's or a `file://` one.
A list is a JSON array of releases, each an object with a `tag_name` such as `v0.14.2-beta`, a `draft` flag, and `assets`, each an object with a `name`, a `browser_download_url` and a `digest` written `sha256:` and the hexadecimal digits; drafts and tags that are not release versions are left out.
A machine's program is the asset named `ens-<system>-<processor>`, as in `ens-linux-x64` or `ens-windows-x64.exe`.
`GITHUB_TOKEN`, when it is set, is sent to `https://api.github.com` to raise its limit on requests, and to no other address unless `ENS_RELEASES_TOKEN` is `always`.
`ens toolchain install <version>` installs a release such as `0.14.2`, the newest release of a language version such as `0.14`, or with `latest` the newest release of all, and leaves a release that is already installed as it is; `--force` downloads it again and replaces the installed copy.
A release is downloaded into a staging folder under the toolchains folder, checked against the sha256 its release list reports, marked executable on Linux and macOS, and moved into its folder in one step, so a download that is refused or stops part-way installs nothing, and two installs of one release at once both succeed.
A download whose sha256 does not match, or that the list reports no sha256 for, is refused.
`ens toolchain remove <version>` removes the toolchain installed under that name, and it is always run by the `ens` that was started, which never removes itself.
A removal takes the version's folder out of the toolchains folder in one step before deleting its files, so a removal that stops part-way never leaves a toolchain that looks installed, and a toolchain that is running while it is removed keeps running.
Every command except `ens toolchain remove` takes `--toolchain <version>`, which selects a toolchain by name whatever is installed and whatever the build root declares, and `--toolchain local` keeps the work with the running toolchain; `ENS_TOOLCHAIN` is the environment spelling of the same choice, and the option wins when both are given.
A name is matched exactly as written, against the running toolchain's own release and against the folder names under the toolchains folder, so on `ens 0.13.0-beta` the name `0.13.0-beta` keeps the work here while `0.13.0` and `0.13` name folders.
A whole release asked for by name, such as `0.14.2`, is downloaded and installed when it is not installed, and asking for anything else that is not installed, such as `0.14`, is an error.
A download prints where it comes from on the error stream, unless `--quiet` is given.
A command line the running toolchain cannot read, such as a command added in a later version, is handed to the newest installed toolchain to be read there, unless it is written with `--toolchain`, which the running toolchain could not read out of it.
The command line reaches the other toolchain exactly as it was written, and a toolchain that was handed the work never hands it on again, so there is at most one hop.

---

Methods that can throw exceptions are marked with `throws`; any other method can be considered safe.
Every thrown value must be an instance of a subclass of `Error`.
`Error` itself is abstract and cannot be instantiated, so the type of every failure a program raises identifies which failure it is.
For most methods the set of throwable types is computed by the compiler and shown by IDEs on hover.
Any function or method may also declare its thrown types explicitly, as in `read() -> bytes throws IOError` or `read() -> bytes throws IOError, ParseError`.
The list is an upper bound: the body may raise those types or their subclasses and nothing else, and callers see the list rather than what the body happens to raise today, so widening it is a deliberate edit.
A list is required on an abstract method, which has no body to infer one from, and where a method can be overridden the list binds every override to the same bound.
A generic may list one of its type parameters, as in `run<E: Error>((() -> void throws E) body) throws E`, provided the parameter has a class bound under `Error`.
Such a parameter stands for a whole throws list rather than for one type: a body throwing `ParseError` and `IoError` binds `E` to both, a body throwing nothing binds it to the empty list, and each caller's contract names exactly what that caller's body throws.
Inside the generic, a call that throws `E` is written with `try` whatever the list turns out to be, so an instantiation whose list is empty does not report that `try` as unneeded.
`catch (E caught)` catches every type in the list, and `caught` has the nearest class every one of them extends, which is `Error` for two unrelated kinds.
Outside, an instantiation whose list is empty throws nothing, so a call of it is written without `try`, as any call that cannot throw is.

If any exception is not handled and the method is not marked as `throws`, this should result in a compilation error explaining which exceptions were not handled and how to handle them (either with a `catch` block or via the `throws` keyword).

These exceptions are always checked. The user can use `panic()` to stop execution, which doesn’t require the use of the `throws` or `throw` keyword.

Methods that throw must be called with `try` as a prefix, even if caught. `Catch` can be added as an additional block of a method.
A `try` written before a chain of calls covers every call in the chain, so `try folder.join(name).writeTextAtomic(text)` needs only that one `try` whichever of its calls can throw, while a call inside an argument needs a `try` of its own.

```ens
class TestRepository {
    getName() -> string? {
        return try this.queryName();
    } catch (DatabaseError e) { // and other catch blocks if multiple exceptions are thrown
        eprint("Database error occurred: {e}");
        return null;
    }

    getNameUnsafe() -> string throws {
        return try this.queryName();
    } catch (DatabaseError e) {
        eprint("Database error occurred: {e}");

        // the current caught exception can be rethrown as-is, rethrow can only be used inside of catch blocks.
        rethrow;
    }

    getNameUnsafeNoCatch() -> string throws {
        return try this.queryName();
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

Every error carries a `message` describing what failed and a `cause`, the error it was raised in response to, or null when it wraps nothing.
Both are `const`, so what an error carries is fixed when it is raised, and a subclass passes them up with `super(message)` or `super(message, cause)`.
An error's text form is its message, then each cause on its own line innermost last, each introduced by `caused by` and the cause's own type name:

```
request failed
caused by OpenError: could not open config.toml
caused by ReadError: no such file or directory
```

The text writes a bounded number of causes and ends with `and further causes` when the chain runs longer.
`stackTrace`, `stackFrames`, and the text of an error's own type are provided by the compiler rather than by any body, so a subclass cannot replace them.

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
    eprint(e.stackTrace());                // the trace as a string

    StackFrame[] frames = e.stackFrames();   // or as structured frames
    StackFrame origin = frames[0];
    eprint("thrown by {origin.function} at {origin.file}:{origin.line}");
}
```

`stackTrace() -> string` returns the same text shown for an unhandled exception. `stackFrames() -> StackFrame[]` returns the frames as values, each a `StackFrame` with `function`, `file`, and `line`; `frames[0]` is the throw site.

At `-O2` and `-O3` a small function, or one the program calls from a single place, may be copied into the function that calls it.
Its frame is still in the trace, with `inlined` after its line, as in `at helper (math.ens:4, inlined)`, ahead of the frame of the function it was copied into.
`stackFrames()` lists it as an ordinary `StackFrame`, so the frames of a trace are the same at every level and only the text marks the copy.

---

Tests are declared with the `test` keyword, a description string, and a body.
Test declarations live in files ending `_test.ens`, next to the code they cover; everything else in a test file (helpers, classes, imports) is ordinary Ens.
Regular builds skip `_test.ens` files entirely, so tests never ship with the program; `ens test` compiles and runs them, and `ens check` checks them along with the sources.

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

The `@std.testing` module provides `TestFailure`, an `Error` subclass whose constructor takes a message and an optional cause, and assertion helpers that throw it:

- `testing.assertEqual(actual, expected)` and `testing.assertNotEqual(actual, expected)` compare two values of the same type with `==`.
  A failure over short values shows both; over multi-line text it names the first line that differs, and over long single-line text the first differing offset with an excerpt around it, so two long values are never printed whole.
- `testing.assertTrue(condition, message)` and `testing.assertFalse(condition, message)` check a condition; the message is optional, and interpolation at the call site can add context (`"sum was {sum}"`).
- `testing.fail(message)` fails unconditionally, and it never returns, so a call to it closes the path it sits on.
- `testing.assertNear(actual, expected, tolerance)` accepts a `double` within `tolerance` of the expected value, for results that arrive through arithmetic rather than exactly; a NaN on either side is never within any tolerance.
- `testing.assertThrows<E>(body)` runs a `(() -> void throws E)` body and returns the `E` it threw, so the test goes on to check its kind or its message; a body that returns without throwing fails the test.

```ens
test "empty input is refused" {
    let failure = try testing.assertThrows<ParseError>(() -> { try parse(""); });
    try testing.assertEqual(failure.message, "nothing to parse");
}
```

`ens test [path] [--tests <folder>] [--filter <substring>]` discovers every `_test.ens` file under the tests folder (a package's `tests/` folder, or the source folder itself when there is none) and runs the tests in a deterministic order: files by path, tests in source order.
With `--tests`, test files live outside the source tree: their imports resolve against the source folder first and then the tests folder, and a module present under both folders is an error.
Each test prints one line, a failing test also prints its message and the stack trace of the failure, and the run ends with a summary:

```
PASS addition adds small integers
FAIL subtraction fails on purpose: expected '1', got '-1'
  at assertEqual (testing.ens:7)
  at "subtraction fails on purpose" (math_test.ens:11)
2/3 tests passed
```

`--filter` runs only the tests whose description contains the substring.
The exit code is `0` when every test passes, `1` when any test fails, and `2` when the tests do not compile or a test run stops before it has run them all.
Test files must have unique file names, and neither a test file nor anything it imports may define `main()`.

At a workspace root every member is tested against its own tests.
Each member's results are announced under its package name, the way a build announces its members, and the run ends with one total across every member that had a test to run:

```
[1/2] demo.lib:
PASS greeting composes
1/1 tests passed
[2/2] demo.app:
FAIL rendering fails on purpose: expected '1', got '-1'
  at assertEqual (testing.ens:7)
  at "rendering fails on purpose" (render_test.ens:5)
0/1 tests passed
1/2 tests passed across 2 members
```

A member whose tests could not run at all is named in that line, and the tests it had still count towards the total as tests that did not pass, so a total is never quietly short.

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

To read through a nullable value, use the safe member operator `?.`. If the value on the left is `null`, the whole expression evaluates to `null` and the right-hand side is not evaluated; otherwise it behaves like `.`. The result is nullable, and a member that is already nullable keeps its own single level rather than gaining a second, so chains such as `a?.b?.c` read through every nullable link.

```ens
Outer? outer = new Outer();
Inner? maybeInner = outer?.inner;   // either null or the field value
int? wheels = car?.wheels;          // value-typed members work too
long size = name?.length ?? 0;
listener?.notify();                 // runs only when listener is present
```

Inside `if (x != null) { ... }` the `x` is considered as the non-nullable form for the rest of the block, so you can use `.` directly.
The same narrowing applies to the `else` branch of `if (x == null) { ... } else { ... }`.
Reassigning `x` inside the block replaces the narrowing with what the new value proves, as the next paragraph describes.

A binding also narrows without an explicit check.
Assigning a value to a local variable or a parameter narrows it to the type of that value from that point on: it drops each level of null the value cannot be, and it takes the value's class or interface where that is narrower than the declared one.
A declaration initializer drops the levels of null the same way, while the local keeps the class or interface it is declared with.
A write to a `const` field through `this` in a constructor also drops only the levels of null.

```ens
string? s;
s = compute();         // compute() returns a non-null 'string'
s.length;              // s is treated as non-nullable 'string' here

Shape shape;
shape = new Circle();
shape.radius;          // shape is treated as a 'Circle' here

Shape? found;
found = findCircle();  // findCircle() returns 'Circle?'
found?.radius;         // found is treated as a 'Circle?' here

string? t = "x";       // starts narrowed from its initializer
Shape other = new Circle(); // a 'Shape', as declared
```

Narrowing, by a check and by a write alike, applies to stable places only.
A stable place is a local variable, a parameter, `this`, a `lazy const` read through its type name, or a `const` field reached through a stable place, however long the chain of them is.
A mutable field and any array element are never stable places, because another reference can write that storage between the check and the use.
A null check on one stays legal but proves nothing for the reads after it, and a write through one refines nothing: `this.field = x` and `arr[0] = x` leave the place nullable.
The idiom is to read the value into a local and check the local, as in `let door = room.door; if (door != null) { door.open(); }`.

Assigning in only one branch of an `if` refines the value below only when every other branch proves it non-null too (see the join rule below): `if (ready) { x = fallback(); }` leaves `x` nullable below, because on the path that skips the assignment nothing has made `x` non-null.

Because narrowing governs only the reads, `== null`, `!= null`, `??`, `?.`, and `?[` on a binding whose declared type is nullable stay legal even where the value has already been proven non-null; the redundant check is simply constant at runtime.
A `?.` there reads the members of the type the value was narrowed to, so `shape?.radius` compiles where `shape is Circle` holds.
A binding whose declared type is not nullable still rejects these operators.

Narrowing also follows the short-circuit operators and conditions: `x != null && x.ready()` narrows `x` on the right of `&&`, `x == null || x.ready()` narrows on the right of `||`, a conjunction of checks narrows the whole `if` branch or ternary branch, and a loop condition narrows the loop body.
A check or a type test of a plain assignment tests the value it stores, so `(x = e) != null` and `(x = e) is T` narrow `x` the way `x != null` and `x is T` do.

```ens
if (a != null && a.b != null) {
    a.b.use();                    // both links narrowed, 'b' being a const field
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

A narrowing holds after an `if`/`else` (or a `switch` or a `?:`) when it holds at the end of every branch that can fall through: the branches are intersected at the merge.
Each branch starts from what holds before it, so a write in one branch does not reach a read in the other.
Where the branches narrow a value to different types, it keeps the wider type where one holds the other, and otherwise the nearest class both extend, or else its declared class or interface.
It can be null after the merge only where some branch leaves it nullable: after `if (round) { shape = new Circle(); } else { shape = new Square(); }`, a `Shape?` named `shape` is a `Shape` that is not null.
A branch that always exits, by `return`, `throw`, or `panic`, and, inside a loop, also by `break` or `continue`, reaches nothing below the merge, so it places no constraint on the result.
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

Narrowing extends to **chains of `const` fields** (`this.limit`, `obj.limit`, `a.b.c` where every field is `const`), with the same `!= null` and `== null` patterns, and the narrowed form holds for as long as nothing invalidates it.
A mutable field or an array element anywhere in the chain keeps it from narrowing, so such a value is read through a local copy.

```ens
if (this.limit != null) {
    return this.limit.length;     // 'limit' is a const field, so the check holds
}

let first = xs[0];                // an element never narrows, so copy it
if (first != null) {
    first.method();
}
```

A `weak` field is never a stable place either, and for a reason of its own: the field can become null whenever the object it refers to loses its last strong reference, so a null check proves nothing about a later read.
Comparing a weak field against null stays legal as an ordinary boolean expression; it simply narrows nothing.
The idiom is the strong-local copy: reading a weak field yields a strong reference, so a local holding it keeps the object alive for the local's scope and narrows by the ordinary rules.

```ens
if (h.target != null) {
    h.target.use();      // error - 'target' is weak, the check does not carry
}

let target = h.target;   // the local holds a strong reference
if (target != null) {
    target.use();        // ok - the local keeps the object alive
}
```

Narrowing is dropped when the analyzer can't prove the narrowed value is still non-null. Specifically:
- Reassigning the narrowed local or parameter, or the one at the root of a narrowed chain.
- Passing the local as an `out` argument, which lets the callee write the caller's variable directly.

A call drops nothing that a stable place proved, whether the place is the call's receiver (`x.method()`, `r.door.open()`), one of its arguments (`use(x)`), or an argument of a constructor (`new T(x)`).
A callee cannot reassign the caller's binding, the binding's own reference keeps the narrowed object alive, and no callee can write a `const` field.

Inside a loop a narrowing must hold on every iteration.
A narrowing established before the loop holds inside it when what each pass leaves for the next one proves it again, at the end of the body, at each `continue` and after a `for` update, as `name = "b";` proves that a `string?` local holds a `string` and `shape = new Circle();` proves that a `Shape` local holds a `Circle`.
A write that proves only part of it keeps that part: storing a `Square` into a `Shape?` local narrowed to `Circle` leaves the local a `Shape` that is not null where the loop starts.
Any other write, such as `name = pick();` where `pick` returns `string?`, or passing the local as `out`, drops the narrowing at the loop's entry: an earlier read in the loop would otherwise use, on the next pass, the value that write stored.
A write after which the pass leaves the loop, by `break`, `return` or `throw`, leaves nothing for the next pass, so it does not drop the narrowing inside the loop.
After the loop, a narrowing holds when it holds everywhere a pass can leave the loop: where the condition is false, which is after any write the condition makes, at the start of each pass of a for-in loop, and at each `break`, so `while (go) { name = "b"; }` leaves a `string?` named `name` a `string` after the loop when it was one before.
A narrowing the loop condition or an in-body guard clause re-establishes on each iteration is unaffected, so `while (x != null)` loops and guard-narrowed loops keep working.

A write to a narrowed local is checked against its declared type, not the narrowed one, so nulling out a just-checked local (`x = null`) or storing a base value over an `is`-narrowed one is allowed, and the local keeps only what the write proves.

```ens
if (room.door != null) {   // 'door' is a const field
    room.door.code;        // ok
    room.door.open();      // a call cannot write 'door'
    room.door.code;        // still ok after the call
}
```

The `is` type test (described with `as?` further below) narrows by exactly the same rules, including these invalidation points.

A `?` suffix always adds a nullable level rather than folding into one that is already there, so `string??` is a type of its own: the outer level marks whether there is a `string?` at all, and the inner one is that `string?`.
That is what keeps "there is no value here" apart from "there is a value here and it is null".
The everyday way to arrive at one is a generic member declared `T?` used at a nullable `T`: `Map<string, int?>` returns `int??` from `get`, so a key that was never stored and a key stored with a null value are different results rather than the same `null`.

```ens
string?? text = "written";        // a plain string fills both levels
string?? held = maybe();          // a 'string?' fills the outer level only
string?? nothing = null;          // the outer level is absent
```

`null` always means the outermost level.
An assignment fills the levels it needs in one step: a `string?` written where a `string??` is expected wraps once, and a plain `string` wraps twice.
A value nullable at more levels than the type it flows into never fits, so it has to be unwrapped first.

`x == null` tests the outermost level only, and one null check strips exactly one level; reaching the value under a `string??` therefore takes two checks.
Once a `string??` named `x` is known to be present at its outer level, it is a `string?`, never a `string`.
This happens after `x != null`, after an assignment of a `string?`, and from an initializer that is a `string?`.
`x == null`, `x != null` and `x ?? fallback` still test that outer level, so reaching the value takes a second check on a copy, as in the example below.
`?.`, `?[`, `is` and `switch` read the `string?`.
`??` unwraps exactly one level: its result is whatever that level held, and its fallback is typed at the level below, so `x ?? fallback` on a `string??` produces a `string?` and uses the fallback only when the outer level is absent.
Chaining is how a value reaches its core, as in `(x ?? null) ?? value`.

```ens
describe(string?? value) -> string {
    if (value == null) {
        return "nothing at all";      // the outer level is absent
    }
    string? inner = value;            // one check took the outer level off
    if (inner == null) {
        return "a null value";        // the inner level holds null
    }
    return inner;                     // the second check reached the string
}
```

Every operator that looks through one nullable level rejects a value that has two, and the error names which level to unwrap first: `?.`, `?[`, `.`, `[`, `for (... in ...)`, `switch`, `is`, `as?`, and interpolation.
`?.` and `?[` are not an exception to that rule and not a way to make one: they flatten the level they would add, so reading a member that is already nullable produces a value at that member's own level and a chain keeps working, while a receiver that is already doubly nullable is still rejected.
A `weak` field stays single-level by rule, `weak T?` over a class and nothing deeper.

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
Applying either operator to a literal, a computed expression, a `const` binding, a `const` field, or a non-numeric value is a compile error.

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

An integer converts to a `char` with `as?`, which evaluates to `char?`: the character whose code point the integer is, or `null` when the integer is negative, past 0x10FFFF, or a surrogate from 0xD800 through 0xDFFF.
A `byte` or a `char` is always a code point, so it converts with `as`, and `as?` from either is an error.

```ens
char? c = code as? char;              // the character, or null
char shown = code as? char ?? '?';
```

The target must be a class or an interface (or, for `as?` only, a numeric enum or `char`); testing against a struct, another primitive, a plain enum, an array, or a string is a compile error, and so is a nullable target like `as? Circle?`, whose result would already be nullable.
The scrutinee must be a class, an interface, or a nullable form of either, and the target must be related to it: a test that could never succeed (unrelated classes) and a test the static type already satisfies (always true) are both compile errors.
A nullable scrutinee tested against a type it already satisfies is the exception: for `Base? x`, the test `x is Base` is a combined null-plus-type check and is allowed.
An interface target over a class scrutinee is an error only in the impossible case, a `final` class that does not implement it (any other class could have an implementing subclass), or the always-true case where the static class already implements it.
An interface scrutinee may be tested against any class or interface target; the outcome is determined by the value's runtime type.

A type parameter may be the target of `is`, `as?`, and a switch `is`-arm, and the test is judged per instantiation against the concrete type argument, following every rule above.
So `value is T` narrows `value` to `T` where it matches, an instantiation whose argument makes the test vacuous or impossible is a compile error where that argument was written, and one whose argument is not a class or an interface is refused the same way.

`if (x is Derived)` narrows `x` to `Derived` inside the branch, following the same rules as null narrowing above: the same places narrow (locals, parameters, `this`, and chains of `const` fields), `x is Derived && x.derivedMethod()` narrows the right side of `&&`, conjunctions narrow the branch, a loop condition narrows the body, and the same writes drop the narrowing.
Failing the test proves nothing about the value's type, so the plain else branch of a positive `is` does not narrow.
An `as?` cast never narrows its operand, because comparing its result with `null` tests the result and not `x`: write `x is Circle` for `(x as? Circle) != null` and `!(x is Circle)` for `(x as? Circle) == null` to narrow `x`.

Negating a check flips what it proves, so the fact that survives is the negated test and a negative guard narrows.
`!(x is Circle)` proves `x is Circle` on its surviving side, which is the else branch and the code following a guard that always exits, as in `if (!(x is Circle)) { return; }` where `x` is a `Circle` on every line below.
Negated null checks are symmetric: `!(x == null)` narrows like `x != null`, `!(x != null)` narrows like `x == null`, and double negation composes.

`is` sits at the comparison precedence level, so `a is Circle && b is Square` parses as `(a is Circle) && (b is Square)`.
`as?` binds tightly to the value just before it, like `as`.
Inside a generic body the scrutinee may have a type-parameter type; the requirements are then checked against the concrete type of each instantiation.

---

A narrower numeric value automatically converts to a wider one when the conversion preserves every possible value. Narrowing always requires casting with `as`.

```ens
int x = 5;
long y = x;              // int -> long, automatic
int n = arr.length;      // error: long -> int, can be forced with `arr.length as int`
```

A nullable number widens the same way under each level of null, so an `int?` converts to a `long?` and an `int??` to a `long??`, and a `null` stays `null`.
Two numbers that meet, as the two results of a `?:` or the two sides of `==`, take the wider type under as many levels of null as either has, so an `int?` beside a `long` gives a `long?`.

Integer literals without a type suffix adapt to the surrounding type when it's an integer that fits the value. With no context they default to `int`. Values out of range produce a specific error.
Where a nullable type such as `byte?` is expected, a literal adapts to the type without the `?`, so `byte? small = 1;` compiles, and so does `small += 1;` right after it.
A literal in parentheses adapts the same way as the literal alone.
A `char` literal beside an integer value adapts to that value's type the same way, so with `byte b`, `b + 'a'` is a `byte` and `b == '€'` is an error, because the code point of `'€'` does not fit a `byte`.
Beside a `char` or an untyped integer literal it stays a `char`, so `'a' + 1` is a `char`.
Comparing an integer value with a floating-point literal that no value of its type can equal is an error, whether the literal is past the range of the type, as in `b == 1e30` with `byte b`, or has a fraction, as in `b == 1.5`.
An ordering is an error only when every value of the type is on the same side of the literal, as in `b < 1e30`, so `b < 1.5` compares as usual.

That default is held to the same rule as any other type, so a literal nothing gives a type to must fit the type it falls back to, and a value past it is an error naming the type that would hold it rather than a silent truncation.
A cast counts as naming a type: a literal whose value the target holds adapts to it, while one the target cannot hold keeps its default and is truncated by the cast, which is what a narrowing cast is for.

```ens
byte b = 5;              // OK - 5 fits in byte
long n = 5;              // OK - 5 fits in long
byte big = 300;          // error: 300 is too large for 'byte', whose largest value is 255
```

Floating-point literals follow the same rule in their own family: one written without a type suffix adapts to the surrounding type when that type is `float` or `double`, and with no context it defaults to `double`.
An unsuffixed integer literal adapts to a floating-point type too, since it names such a value just as well: `float half = 5;` holds `5.0`.
Rounding to the nearest value the type has is part of the conversion, so `float ratio = 0.1;` is accepted even though no binary floating-point type holds a tenth exactly, and `float count = 16777217;` stores `16777216`, the nearest value a `float` has.
Only a magnitude the type cannot hold at all, one that would become infinity, produces an error; a magnitude closer to zero than to the type's smallest nonzero value rounds to zero.
A suffix names the type and leaves nothing to adapt, so `1.0f` is a `float` and `1.0d` a `double`, just as `5L` is a `long`.

In both families a literal takes its type from what it sits against, and the surrounding type does not reach through an operator into its operands.
So `uint mask = 1 + 4;` and `float ratio = 1.0 + 2.0;` are both errors; give one operand the type you want, as in `float ratio = 1.0f + 2.0;` or `uint mask = (1 as uint) + 4;`, and the other adapts to it.

```ens
float ratio = 0.5;       // OK - the literal adapts to float
float tenth = 0.1;       // OK - rounds to the nearest float
double wide = 0.5;       // OK - a literal with no other context is a double
float suffixed = 1.0f;   // OK - written as a float outright
float tooBig = 3.5e38;   // error: too large for 'float', whose largest value is about 3.402e38
float viaOperator = 1.0 + 2.0;  // error: the operands are doubles, and their sum is a double
```

---

`let` and a typed declaration both introduce a mutable binding. `const` introduces an immutable one: it must be initialized, and assigning to it again, or passing it as `out`, is a compile error. Like `let`, a `const` may infer its type or state it explicitly.
Writing a field of a `const` local of a struct type, at any depth, is an error too.
So is calling a `mutating` method on one.

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

An assignment is itself an expression, and its value is the value its target holds after the write.

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

`while` repeats its body while the condition holds. A `for` loop comes in two forms. The C-style form has an initializer, a condition, and an update, any of which may be omitted; the initializer is scoped to the loop. The for-each form walks an array element by element, or any iterable value one element at a time.

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

When the elements are structs, the loop variable is a copy of each element, and writing a field of it or calling a `mutating` method on it is an error, because the array would not change.
To change the elements, loop over the indexes with the C-style form and write `xs[i]`, as in `xs[i].count += 1;`.

A class or a struct is iterable when it implements the `Iterable<T>` interface from `@std.collections.iterator`, directly or through an interface that extends it; its single method `makeIterator() -> Iterator<T>` returns an `Iterator<T>`, an interface with the single method `next() -> T?`.
Iterating a struct needs no interface value, because the loop calls `makeIterator()` on the struct itself; the iterator it returns is a class or an interface value as any other iterator is.
The loop calls `makeIterator()` once, then draws values with `next()` until it returns null.
The same rule applies to the structs an iterable value produces, since the variable is a copy of what `next()` returns.
`next()` returns the value it moved onto, or null once the walk is over, and every call after that returns null too.
When the element type is itself nullable, `next()` returns a nested optional, so a `null` held by a present result is an element and only an absent result ends the loop.
A value whose static type is `Iterable<T>` itself, or an interface extending it, can also be iterated.
A `string` cannot be iterated directly: walk `text.chars()` for its characters or `text.bytes()` for its bytes, and the loop takes the array that walk produces.

```ens
import Iterable from @std.collections.iterator;
import Iterator from @std.collections.iterator;

class Range implements Iterable<int> {
    int low;
    int high;

    constructor(this.low, this.high);

    override makeIterator() -> Iterator<int> {
        return new RangeIterator(this.low, this.high);
    }
}

class RangeIterator implements Iterator<int> {
    int current;
    int high;

    constructor(this.current, this.high);

    override next() -> int? {
        if (this.current > this.high) {
            return null;
        }
        int value = this.current;
        this.current = this.current + 1;
        return value;
    }
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
The distance never determines the result, so an untyped literal on the left of a shift stays `int` and shifting by a `uint` distance is written `(1 as uint) << spread`.

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
byte bits = mask & 300;         // error: 300 is too large for 'byte', whose largest value is 255
bool ready = ~true;             // error: '~' flips the bits of an integer, but 'true' has type 'bool'
```

The logical operators `&&` and `||` require `bool` operands and short-circuit: the right side is evaluated only when it can change the result, so `a != null && a.ready()` is safe.

The null-coalescing operator `??` takes a nullable value on the left. It evaluates to that value when it is not `null`, and otherwise evaluates and returns the right side. The right side is skipped when the left is non-null. Its two `?` characters must be adjacent, because a `?` is also the nullable type suffix; `a ? ? b`, with anything at all between them, is a malformed conditional rather than this operator.

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
    print("running {command}");   // running Submit
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

A switch is also an expression: each arm yields a value or never returns, the arms that yield one unify to a common type (the same way the branches of `?:` do), and the switch evaluates to the matched arm's value.
In statement position an arm's body may be a `{ }` block; used as a value, each arm is a single expression.

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

A lambda, an aggregate literal, an empty `[]` or an array literal holding one of them, written as a branch of `?:` or an arm of a switch used as a value, takes its type from the context the whole `?:` or switch is written in, as it would if it were written there itself.
That context is the declared type of a variable, a parameter, a return type, an assignment target, an array element or a field of an aggregate literal, and for the right side of `??`, the left side's type without its `?`.
So in `run(ready ? (x) -> x + 1 : (x) -> x * 2)` both lambdas take the type of `run`'s parameter, and among overloads the call picks the one such a `?:` fits, as it would for each of its lambdas written alone.
A branch with a type of its own keeps it, so `((int) -> int)? step = ready ? (x) -> x + 1 : null;` gives the lambda the type `(int) -> int`.
The `?:` or switch itself still has the type its values unify to, as above: that `?:` has the type `((int) -> int)?`, and in `Point? p = ready ? {x: 1, y: 2} : {x: 3, y: 4};` the `?:` has the type `Point`, so `p.x` needs no null check after it.
With no context type, such a value takes the type the other branches unify to, as a number literal does, so in `let step = ready ? twice : (x) -> x + 1;` the lambda takes the type of `twice`.
Where nothing gives those values a type, as in `let step = ready ? (x) -> x : (x) -> -x;`, it is an error, and so is a type they cannot take, such as `int` for a lambda, since the branches then have no common type.

```ens
run(ready ? (x) -> x + 1 : (x) -> x * 2);            // both lambdas take the type of run's parameter
Point corner = ready ? {x: 0, y: 0} : {x: 1, y: 1};  // both literals build a Point
let step = ready ? twice : (x) -> x + 1;             // the lambda takes the type of twice
let guess = ready ? (x) -> x : (x) -> -x;            // error: nothing gives the lambdas a type
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
    Initialize -> { print("starting"); },
    default -> print("ignored"),
}

let length = switch (name) {      // name is string?, the null case is handled
    "hi" -> 2,
    null -> -1,
    default -> 0,
};
```

A switch over a class or interface value matches on the runtime type instead of on labels.
A type arm is written `is Type binding -> body`; the binding is an arm-scoped constant of type `Type`, and it may be omitted when the value is not needed.
An arm without a binding narrows the switched value to `Type` inside the arm when that value is a place that narrows (a local, a parameter, `this`, or a chain of `const` fields), and the writes that drop any narrowing drop this one.
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

Expressions, types and blocks may only nest so deeply inside one declaration, and nesting past that limit is a compile error.
A chain of `else if` branches counts toward the same limit.

---

Arrays are written with `T[]` and are reference types: declaring an array variable binds a pointer to a heap allocation, and copying the variable copies the pointer.
Two arrays of the same type compare with `==` and `!=` by content: unequal lengths are unequal, and equal lengths compare element by element, each element by its own `==`, so IEEE rules hold for float elements, strings compare by content, classes by identity unless their run-time class opted into content equality, external handles by identity, and nested arrays recurse.
An element type with no `==` of its own, such as a function value, makes the comparison an error naming that type.
An array cannot be a `Map` or `Set` key, because its contents, and so its hash, can change while it sits in the table.
The same holds for a `List`, `Map`, or `Set` used as a key, and for a struct key whose fields, at any depth, hold an array or a collection; the error names the field.
An external handle or a function value has no hash at all, so neither can be a key, and calling `hash()` on one, directly or through a type parameter, is an error naming the type.
A `float` or a `double` has no hash either, because equality and hashing disagree on two of their values.
A `NaN` is equal to no value, so nothing could find it again, and negative zero is equal to zero but hashes differently, so one key would become two entries.
A struct whose fields hold one of them at any depth gets its hash back by declaring its own `equals` and `hash`, comparing each such field with `compareTo` and hashing it with `toCanonicalBits`.
A `NaN` then matches a `NaN`, and the two zeros stay two keys, as a `SortedMap` orders them.

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
  An element type that is a type parameter is judged once per instantiation the program actually uses, and the refusal is reported where that instantiation's type arguments were written, because whether a freshly allocated slot is a valid value depends on the type argument.
- `new T[a][b]` allocates a fully-populated multidimensional grid in one call: an outer array of length `a`, each slot holding a freshly-allocated `T[]` of length `b`. The same shape extends to higher dimensions (`new T[a][b][c]`). Because every intermediate level is allocated, types like `int[][]` are valid here even though no intermediate slot is nullable.
- `new T[a][]` allocates only the outer array; inner slots stay `null`. The result type is `T[]?[]`, the deepest unallocated level is reflected in the type by adding a `?`. Trailing empty brackets compose: `new T[a][b][]` produces `T[]?[][]`. Sized brackets must come before any empty ones in a single `new` expression.
- `arr[i]` reads or writes an element. Bounds are checked at every access; an out-of-range index aborts the program.
- `arr.length` returns the number of elements as a `long`.
- `arr.slice(start, end)` returns a new array holding the elements in the half-open range `[start, end)`.
  An invalid range aborts the program.
  Elements are copied the way ordinary assignment copies them, so class elements are shared by reference.
- `T?[]` is an array of nullable `T`, each **element** can be `null`. `T[]?` is a nullable array variable. The variable itself may be `null`. The two compose: `T?[]?` is both. To safely index a nullable array, use `?[i]`: it short-circuits to `null` when the receiver is `null`, otherwise it indexes normally.
  The `?` and `[` must be adjacent, with nothing at all between them, because a `?` also begins a conditional expression whose first result may be an array literal.
  So `rows?[0]` indexes, while `cond ? [1, 2] : [3, 4]` is a conditional choosing between two array literals.

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

An empty literal `[]` requires a target type, or as a branch of `?:` or an arm of a switch, another branch with a type of its own; `let xs = []` is rejected because there is no element to infer from. Write `int[] xs = []` or pass `[]` as an argument where the declared parameter type pins it down.

An array literal is one way to construct an array of a non-nullable reference type: `[new Box(1), new Box(2)]` initializes every slot at construction, and the resulting type is `Box[]`.

```ens
let bs = [new Box(1), new Box(2)];     // bs: Box[]
makeBoxes() -> Box[] {
    return [new Box(1), new Box(2)];
}
```

When the size is a run-time value, `new Box[n]` is legal in exactly one shape: declare a new variable with the allocation, and make the very next statement a loop that fills every slot.
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
A `char` literal has exactly one character or one escape between single quotes, as in `'a'` or `'\n'`, so `''` and `'ab'` are compile errors.
That one character is one code point, so a letter written with a separate combining accent is two characters and needs a string.

- `==` and `!=` compare **contents**, not identity, so `"ab" == "a" + "b"` is true.
- `s.length` returns the number of UTF-8 **bytes** as a `long`.
- `+` concatenates strings. When one side is a string, a number (integer, `char`, or floating-point) or a `bool` on the other side is converted to text implicitly (the same way `.toString()` would). Every other type is rejected here, structs and classes included even though they have a text form; interpolate those or call `.toString()` instead.
- `.toString()` produces a string from a value explicitly: integer types format as decimal, floating-point types by the rule below, a `char` as the one character it denotes (its Unicode scalar encoded as UTF-8 bytes, so `'A'` is `"A"` and `'7'` is `"7"`, not their code points; write `c as int` first for the number), `bool` as `true` or `false`, a string returns itself, a struct produces its JSON form or what its own parameterless `toString` returns, a class or interface value produces what its runtime type's parameterless `toString` override returns, or that type's name when no class in its chain declares one, and an array produces the same JSON-style list interpolation renders.
  It can be written directly on a literal, as in `42.toString()`.
- Everything else text can do is a member the standard library declares, described with the rest of the library: searching, substrings, trimming, splitting, case conversion, padding, `toBytes` and `string.fromBytes`, and the character and byte views.
  Strings have no `<`, `<=`, `>` or `>=` operators; order them with `compareTo`, which the library declares as well.

A `float` or `double` renders as decimal text that comes back as exactly the same value, so nothing is lost between printing a number and reading it again.
The text is kept short where that costs nothing, but it is the reading back, not the shortness, that is guaranteed.
Concretely a `double`'s text carries the fewest of 15, 16, and 17 significant digits that is still read back exactly, with trailing zeros left off: `2.5` is `"2.5"` and `0.1` is `"0.1"`, while `0.1 + 0.2` is `"0.30000000000000004"` because that sum genuinely is not three tenths.
A `float`'s text carries the fewest of 6, 7, 8, and 9 significant digits that is read back as that same `float`, so it is the shortest text for the value the type actually holds: `0.1f` is `"0.1"` rather than the `"0.10000000149011612"` its widening to `double` would spell, and `1.0f / 3.0f` is `"0.33333334"`.
A value with nothing after its decimal point loses the point too, so `3.0` is `"3"` and `0.0` is `"0"`; negative zero keeps its sign as `"-0"`.
Exponent form is used when the value's decimal exponent is below -4 or reaches the number of significant digits shown, and is written as `e`, a sign, and at least two digits: `1e-5` is `"1e-05"` and `1e21` is `"1e+21"`, while `0.0001` and `1234567` are written out in full.
The three values that are not numbers render as `"inf"`, `"-inf"` and `"nan"`, whatever the C library underneath would have called them.

A `float` and a `double` also have a natural order, which is what `compareTo` returns and what a sort with no comparison given uses, and it gives every value a place, including the ones the operators leave unordered.
In that order a `NaN` sits above every number, infinities included, and level with another `NaN`, and negative zero sits just below zero.
The operators keep their IEEE meaning, so `==` calls a `NaN` unequal to itself and the two zeros equal, and `<`, `<=`, `>` and `>=` all return false when either side is a `NaN`, while `!=` returns true.

`isNaN()`, `isFinite()` and `isInfinite()` are the three tests the library provides for a `float` or a `double`, and exactly one of them returns true for any value.
Every number is finite, both zeros and the subnormals included, and is neither of the other two.
Only `isInfinite()` returns true for the two infinities, and only `isNaN()` for a `NaN`.

`toBits()` returns the bits a value is made of, a `uint` for a `float` and a `ulong` for a `double`, and `float.fromBits(bits)` and `double.fromBits(bits)` read them back.
Every pattern spells a value, so none of them is refused, and a value read back out of its own bits is the value it came from, a `NaN`'s payload included.
`toCanonicalBits()` returns the same bits with every `NaN` written as one pattern and every other value left as it is.
That pattern is `0x7FF8000000000000` for a `double` and `0x7FC00000` for a `float`.
Two values `compareTo` calls equal therefore have the same canonical bits, and the two zeros have different ones, which is what a struct hashing a floating-point field needs.

```ens
let greeting = "Hello, " + name + "!";
let n = greeting.length;            // long, the byte count
if (name == "world") { /* ... */ }
let label = count.toString();       // "0", "42", "-7"
let raw = greeting.toBytes();       // bytes, from the library's member
let at = greeting.indexOf("llo");   // 2, likewise
if (name.compareTo("world") < 0) { /* name sorts first */ }
```

**Interpolation** embeds expressions in a string with `{ }`. Each hole is converted to text the way `.toString()` would, then the literal parts and holes are joined into one new string. Write `\{` and `\}` for literal braces.

```ens
let report = "Area: {width * height} for {width}x{height}";   // "Area: 100 for 20x5"
let status = "done={finished}, items={count}";                // bool and integer holes
let braces = "use \{these\} verbatim";                        // "use {these} verbatim"
```

Holes accept string, integer (including `char`), floating-point, `bool`, and enum values, structs whose fields are all serializable (rendered as JSON) or that declare their own parameterless `toString`, class and interface values, rendered from the runtime type: its parameterless `toString` override, or its type name when no class in the chain declares one, and arrays of any of these, where a struct element's fields must all be serializable even when the struct declares its own `toString`; convert other types explicitly with `.toString()` first.
An array renders as a JSON-style list, `[` its elements joined by `, ` and `]`: each element as struct serialization would write it, so strings arrive quoted and escaped, an absent nullable element is `null`, and a struct element is its JSON object even when the struct declares its own `toString`, exactly as a struct nested in another struct is.
A class or interface element renders through its runtime type's parameterless `toString`, and a struct element whose fields have no JSON form makes the array an error naming the field, whatever `toString` the struct declares.
A hole may hold one nullable level over any of those: it renders the value's own text where the value is there and `null` where it is not, so a `string?`, an `int?`, or a nullable struct, class, or enum needs no check first. A value nullable at more than one level is an error, because every absent level would read the same.
A `char` hole renders as its character rather than its numeric code point, so `"{'A'}"` is `"A"`; interpolate `c as int` when the number is wanted.
Inside a generic body a hole may hold a value of a type-parameter type; the requirement is then checked against the concrete type of each instantiation.
An explicit `.toString()` on a type-parameter value is checked the same way, against the concrete type of each instantiation.
A hole or `.toString()` call whose type has no text form for any type argument, such as a struct with a class field or a function value, is refused in the generic body itself, and no instantiation reports it again.

---

`bytes` is immutable binary data, to a `byte[]` what a `string` is to text: a reference type whose contents never change, so a variable binds a reference, copying it copies the reference, and every operation that "changes" one returns a new one.
It is what whole contents are handed around as, where a `byte[]` is what a stream fills: `readAll()`, `readBytes()`, `toBytes()` and `embed` return `bytes`, `read(buffer)` takes a `byte[]`, `write` takes `bytes`, and `writeBytes` accepts either.
`length` counts the bytes and `data[index]` reads one, with an index outside the data aborting the program, while writing one is refused when the program is compiled, as is passing a `bytes` where a `byte[]` is expected or the other way round.
`slice(start, end)` and `slice(start)` return a half-open range as new data, copied from the original.
`isEmpty()`, `contains(needle)`, `indexOf(needle)`, `indexOf(needle, from)`, `lastIndexOf(needle)`, `startsWith(prefix)` and `endsWith(suffix)` search exact bytes under the same rules as the string members of the same names.
`toArray()` returns the bytes as a new `byte[]`, `bytes.of(array)` returns `bytes` holding an array's contents, moving the storage when nothing else refers to the array and copying it otherwise, and `string.fromBytes(data)` decodes it as UTF-8.
Two `bytes` compare and hash by content, and a `bytes` can be a key, a field of a shareable struct, and a value sent to another task, because nothing can write it.
A literal of type `bytes` is written as an array literal where `bytes` is the target, as in `const bytes signature = [0x89, 'P', 'N', 'G'];`.

---

`embed "path"` is an expression of type `bytes` holding the contents of the file at `path`, read when the program is compiled.
`embed folder "path"` is an expression of type `EmbeddedFile[]` with one element for every file under the folder, at any depth.
`EmbeddedFile` is a struct from `@std.embed` with the file's `path` inside the folder, written with `/` between folders, and its `contents` as `bytes`.
The elements are sorted by `path`, and a file or folder whose name starts with `.`, such as `.git`, is left out.
The path is a string literal relative to the folder of the source file that writes it, with `/` between folders on every system.
An empty path, an absolute one, or one written with `\` is an error, and so is a path that names nothing, or a folder where `embed` expects a file, or a file where `embed folder` expects a folder.
The bytes are part of the program, and every evaluation returns the same `bytes` value without copying, because nothing can write it.
An `embed` is not a compile-time constant, so a `static const` cannot be initialized with one.

```ens
import EmbeddedFile from @std.embed;

final class Assets {
    lazy const bytes logo = embed "logo.png";
    lazy const EmbeddedFile[] icons = embed folder "icons";
}
```

A package embeds the files inside its own folder.
A file outside it can be embedded only from a folder its `ens.package` lists with an `embed` line, described with the other manifest lines.
A program that no `ens.package` declares embeds the files inside the folder it is built from, which is the folder of the workspace around it when there is one.
`embed` and `folder` are ordinary names everywhere except where `embed` is followed by a path or by `folder`.

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

- `external type Name;` declares an opaque foreign handle. The handle is passed around and compared with `null`, but it has no members. Two handles compare with `==` and `!=` by identity, whether directly, as struct fields, or as array elements, and a `Name?` compares with a `Name` the same way. It is private to its file by default and may be marked `public` to share it with the rest of the package; it can never be `export`ed, because a foreign handle has no meaning outside the package that binds it, so wrap it in an Ens type to cross a package boundary.
- `external from libname { ... }` groups foreign function signatures. The name is an identifier naming a native library declared in the package's `ens.package` manifest (here `native kernel32;`); using an undeclared name is an error, and the linker links the library the manifest declares (see the packages section). `libc` is the platform C runtime, declared `native libc system;` and linked by default. An external block and its functions are always private to their file and take no visibility modifier; share their behavior by wrapping the calls in Ens functions.
- The `out` modifier marks a parameter the C function writes back to. At the call site, the caller passes an initialized local variable as `out name`. The variable's type must match the declared parameter type exactly.
- A `string` argument is converted automatically to a NUL-terminated UTF-8 buffer at the call boundary. The C function must not retain that pointer past the call.
- To read a NUL-terminated C string that a C function hands back as a foreign handle, call `fromCString` from the `@std.ffi` module: it accepts a value of any external type and copies the bytes the handle points to into a new Ens string, returning `string?`. A null handle returns `null`, so an unset value stays distinct from an empty one. Freeing the C buffer stays the caller's responsibility.

Memory is managed automatically through Automatic Reference Counting (ARC).

- **Classes** are heap-allocated reference types. Each instance carries a refcount; when the last reference goes out of scope, the instance is freed.
- **Structs** are value types. They are copied on assignment and passed by value.

Atomics happen only at allocation, at assignment of class-typed fields, at scope exit, and around a value read out of a local, a field or an array element when code that runs while the value is still in use could overwrite where it was read from.
Such a value is retained until its use is done: an argument or a receiver until its call returns, a value in an interpolation until its text is made, the value a type switch tests until the switch ends, the variable of a for-each loop over an array until its iteration ends, and an operand until its operator is applied.
A local is overwritten only by an assignment to it or by a `mutating` method called on it.
A field or an array element can also be overwritten by any code that runs meanwhile, such as a call, a constructor, a destructor, a user's `equals`, `hash` or `toString`, the first read of a `lazy const`, or the default of a field a struct literal leaves out.

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

`weak` fields must be nullable class types. They don't contribute to the strong refcount, so they don't keep objects alive. When the referenced object dies, reading any weak reference to it returns `null`.

A null check on a weak field does not narrow it, because the field can go null the moment the referenced object loses its last strong reference.
Read the field into a local and check the local instead: the read produces a strong reference, and the local keeps the object alive while it is in scope.

The compiler performs **escape analysis** to elide retain/release pairs and large-struct copies when it can prove a value does not escape its scope.

`--explain-arc` reports that work.
A function appears in the account when there is something to report about it: which of its incoming references may outlive the call or have their storage written, and one line for each optimization pass that removed reference-counting operations from it, with the number removed.
A last line gives the total for the whole program.
It also names each `frozen()` call that copied its container rather than moving its storage, because a freeze that copies has a second reference behind it worth finding.
The account describes an optimization that ran, so `-O0` prints nothing, the optimizer being off there, and `ens check` prints nothing, since it stops before code generation.

---

A program runs its work on tasks.
A task runs an ordinary function from its start to its end, waiting wherever the function waits, and there is no second kind of function for it: nothing on a signature says that a function may wait, and nothing is written at a call to let it.
Which thread a task runs on is the runtime's business, so a program never names a thread; `@std.tasks` starts tasks, waits for them and stops them, and `@std.parallel` runs one body over many pieces of data at once, both described with the library.

What one task may hand to another is decided by one rule: a value crosses to another task only when nothing can write it, or when every write to it is synchronized.
Such a value is called shareable, and the keyword for it is `shared`.
`shared` is a keyword only where it marks a declaration, a function type or a type parameter, so a variable, a method, a type or a folder may still be named `shared`.
Primitives, enums, `string` and `bytes` are shareable.
A struct is shareable when every field of it is, judged over its fields as `==` and its text form already are.
An array, a `List`, a `Map`, a `Set` and every other container is not shareable, because it can be written through any reference to it; a frozen copy of one is, as the collections describe.
A class is shareable only when it is declared `shared`: it is then `final`, or `sealed` with every subclass declared `shared`, every field of it is `const`, and every field's type is shareable, and a declaration that breaks one of those is refused naming the field.
A class the library synchronizes inside, such as `Mutex`, `Atomic`, `Channel`, `Sender`, `Task` and `TaskScope`, is declared `shared` by the library and is shareable although what it holds changes.
An interface is shareable when it is declared `shared interface`, which only `shared` classes and structs whose fields are all shareable may implement, so a value typed by it can cross.
An `external` handle is shareable only when its declaration says `shared`, because the library behind it is what decides whether two threads may use it at once.
A function value is shareable when its type is written with `shared` before the parameter list, as in `shared (Request) -> Response`, and a lambda written against such a type may capture only shareable values: a local or a parameter of a shareable type, a `const` field of `this` whose type is shareable, which it copies, and `this` itself only when its class is `shared`.
A type parameter is shareable when it is declared `<shared T>`, every argument for it must then be shareable, and the body may treat a `T` value as shareable, so `Channel<shared T>` carries shareable values and nothing else.
A `shared` generic class declares `shared` every type parameter a field's type mentions, since the field could not be shareable otherwise.
`shared (int) -> bool` and `(int) -> bool` are two types: a value of the first is accepted where the second is expected, since a function another task may run is also one this task may run, and a value of the second is not accepted where the first is expected.

```ens
shared class Config {
    const string host;
    const int port;
    const FrozenList<Route> routes;
}

shared interface Handler {
    handle(Request request) -> Response throws IoError;
}

serve(Config config, shared (Request) -> Response handler) { /* ... */ }
```

The rule is checked in three places: at a lambda a task runs, at a value sent through a channel or kept in a `Mutex` or an `Atomic`, and at a `lazy const`, which every task can read by name.
A capture, a send, a store or a `lazy const` of a type that is not shareable is an error naming the value and its type and saying what to write: `frozen()` for a container, a `Mutex` around a value that has to change, or the values the task should return instead.

What a task returns is not held to the rule: the task has finished when its value is taken, so the value belongs to whoever takes it, whatever its type, and a task may build a syntax tree, a list or a map and hand it back.
A frozen copy, a `Mutex`, an `Atomic` and a task's result are therefore the ways writable data moves between tasks, and the two things the rule refuses are these: a writable object is never captured by a task that is already running, and nothing a task started outlives the scope it was started in.
A task may also lend what it owns for exactly as long as it waits: the loops of `@std.parallel` park the calling task until every piece of work is done, so an array only that task could write is handed to the pieces in disjoint parts to write, or whole to read, with no copy and no further rule.

Reference counts are already shared between threads, so a shareable object may be held by any number of tasks at once, and a destructor runs on the task that released the last reference, which is the only task that could still reach the object.
The initializer of a `lazy const` runs once however many tasks read it first at the same moment, and the others wait for its value.
An object a `lazy const` holds is kept to the end of the program, so its reference count is never changed again once it is built, and reading it costs no task a write to it.

---

The standard library is an external package, imported with `@`, and is opt-in apart from the implicitly imported `@std.core` and `@std.io.print`: its other exported declarations are visible only after they are imported.

The standard library also declares members of the primitive types, so what a `string`, a `bool`, a `char`, or a numeric type can do is written there in ordinary Ens source rather than built into the compiler.
Such a member is called like any other, on a literal as readily as on a variable, and a static of a primitive is reached through the type name, as in `string.fromBytes(bytes)`.
A few of those members are the compiler's own work, such as reading one byte of a string, and the library declares them without a body rather than writing them out; which member is which makes no difference at a call site.
Reaching one from another package follows the ordinary visibility rules, so a program sees the members the library exported and nothing else.
The modules declaring these members are read for every program, so a value reaches them with no import written.
The library may also declare that a primitive implements an interface, which is how a primitive satisfies a generic bound: a `<T: Comparable<T>>` parameter accepts `string` once `string` is declared to implement `Comparable<string>`.
That conformance is a compile-time judgment and nothing more, because a primitive value is never wrapped in an object.
Storing a primitive in an interface-typed variable, field, array element, or parameter is an error, and so is `is` or `as?` against an interface; each message names the bound the conformance does serve.
Only the standard library declares a primitive's members, and writing `primitive` in a program is an error.
The word stays an ordinary identifier everywhere else, so a variable, field, parameter, method, or function may be named `primitive`.

The library is a set of module families under `@std`, and an import names a module rather than a family, so the chapters below give the module every type and function lives in.
`@std.core` and `@std.io.print` are the two modules the compiler imports for every program.
The families are `@std.collections` for the walking contracts and the containers, `@std.text` for the members of `string` together with `StringBuilder` and the parsers, `@std.io` for byte streams and the streams a program starts with, `@std.fs` for paths and files, `@std.environment` for what surrounds a running program, `@std.process` for starting other programs, `@std.platform` for `Platform`, `@std.thread` for a wait, `@std.testing` for what a test throws, and `@std.ffi` for reading a C string.
A family's own name is a module too where one type stands at its head, which is why `@std.fs` is where `Path` lives while `@std.collections` is a prefix alone.
`@std.system` exports nothing: it holds the library's own calls into the operating system, which are internal to the library rather than part of what a program writes.

---

`@std.io` moves bytes, and two contracts from `@std.io.streams` describe every stream in it.
`Reader` declares `read(byte[] buffer) -> long throws IoError`, which fills as much of the buffer as it has ready and returns how many bytes it wrote.
That count is `0` only once the stream has ended, so a count shorter than the buffer is a reason to call `read` again, not a sign that the bytes have run out.
`Writer` declares `write(bytes data) throws IoError`, which takes all of `data` and keeps nothing, and `flush() throws IoError`, which hands over everything accepted but not yet passed on.
A `byte[]` is written by passing `bytes.of(array)`.
`streams.copy(source, target)` moves everything the source has left into the target and returns how many bytes moved, flushing and closing neither, so flushing and closing the target is up to the caller.

`@std.io` itself provides the streams a program starts with: `io.out()` is standard output as a `Writer`, `io.err()` is standard error, and `io.in()` is standard input as a `BufferedReader`.
Standard output is buffered, so a line written there reaches the operating system once that buffer fills or something hands it over, and standard error is not, so what a program reports survives a crash.
The buffer belongs to the stream rather than to the writer, so it makes no difference which writer returned by `io.out()` a program keeps.
`io.in()` returns the same reader every call, because bytes read ahead cannot be put back and two readers of one input would steal from each other.
`io.flush()` hands over everything accepted by `io.out()` and not yet given to the operating system.

`print(message)` and `eprint(message)` come from `@std.io.print`, which every program has with no import written, and each writes its message and a newline, the first to standard output and the second to standard error.
Neither throws: printing a line is not an operation a program should have to handle the failure of, so a write the operating system does not carry out is dropped.
What `print` writes is buffered, so a line it wrote may still be inside the program when the next statement runs, but the order a program can observe is guaranteed: everything printed before an `eprint`, before a write to `io.err()`, before a child process starts, and before a panic is reported has left the program first, whichever stream those go to and whether the streams are a terminal or the same file.
A panic therefore never loses what was printed before it.
`io.flush()` is that same guarantee on demand, for an order that only the program itself relies on.

A stream that fails raises an `IoError` from `@std.io.streams`, whose `kind` is the condition a program acts on: `Closed` for a stream that is no longer open, `Interrupted` for a call the system cut short before it moved anything, and `Other` for what the message alone describes.
Beside the kind, `nativeError` carries the number the system itself reported, which a program logs rather than acts on.
It is the C library's `errno` wherever the failing call was the C library's, which is every stream but a child's pipes; a child's pipes are the system's own, so a write to one reports a Win32 number on Windows and an `errno` everywhere else.
It is `0` where no call to the system was made, as it is for bytes that spell no character.
The kinds name only what a stream can establish, so a number with more than one cause behind it arrives as an `Other` with that number beside it.
A condition that only a call on a path can report, such as a file system with no room left, reaches a caller holding a `Writer` as an `Other` too, since only the path identifies what the stream is a stream of.

`BufferedReader` and `BufferedWriter` from `@std.io.buffered` turn many small reads and writes into few large ones, each wrapping one stream and taking a capacity that defaults to 8192 bytes, where a capacity below one byte is raised to one.
`BufferedReader` adds the reads a raw source cannot offer.
`readLine()` returns the next line without its ending, and `null` once the stream has ended; a carriage return before the newline belongs to the ending, and a last line that ends in no newline is a line of its own.
`readAll()` returns everything left as `bytes`, and `readAllText()` returns it as text, raising an `IoError` for bytes that spell no character, since text in Ens is always valid UTF-8.
That refusal is an `IoError` of kind `Other` naming the offset where the bytes stop spelling text, and it carries the `EncodingError` underneath as its cause.
`BufferedWriter` passes what it holds on when it fills, when `flush` is called, and when the wrapper is dropped, and the flush a drop does discards any failure, so a program that must confirm the bytes arrived calls `flush` itself.

`TextWriter` from `@std.io.text` writes text onto a byte stream as that text's UTF-8 bytes: `write(text)`, `writeLine(text)`, which adds a newline and never a carriage return, `writeLine()` for an empty line, and `flush()`.
Each call reaches the target once, so a line written through it arrives whole rather than in pieces something else could get between.
It is not a `Writer` itself, because it is not a destination for bytes.

`BytesReader` and `BytesWriter` from `@std.io.memory` are streams over bytes already in memory, which is what drives anything written against `Reader` or `Writer` with no operating system taking part.
A `BytesReader` hands out the `bytes` it was given, in order, sharing that storage rather than copying it, and returns `0` for ever once the last of it is gone.
Given a `byte[]`, it copies the array once when it is made, so changing the array afterwards leaves what is still to be read as it was.
A `BytesWriter` collects everything written into one growing array, which `toBytes()` returns as `bytes`, a copy so that the writer can go on, `length()` measures, and `clear()` empties.
Neither of them can fail, so a call on either needs no `try`.

```ens
import @std.io;
import BufferedWriter from @std.io.buffered;
import IoError from @std.io.streams;
import TextWriter from @std.io.text;

report(string[] names) throws IoError {
    let lines = new TextWriter(new BufferedWriter(io.out()));
    for (let name in names) {
        try lines.writeLine(name);
    }
    try lines.flush();
}

askName() -> string throws IoError {
    print("your name?");
    string? typed = try io.in().readLine();
    return typed ?? "";
}
```

---

`@std.fs` holds `Path`, the struct every operation on the file system is a method of.
A path is built from text, as in `Path("src/main.ens")`, and `/` separates the parts of a path on every platform.
Text written the way one operating system writes a path arrives through `Path.fromNative(text, platform)`, which turns each `\` into `/` on Windows and keeps the text exactly everywhere else, where `\` is an ordinary character in a name and converting it would name a different file.
`toString()` returns the text the path was built from, and `compareTo` orders paths by that text, which is what `Path` implements `Comparable<Path>` with, so a list of paths sorts and a `SortedMap<Path, V>` keys by them.
Two paths are equal only when their text is equal, so `a/b` and `a/./b` name the same place and are different paths until they are normalized.
The empty path is a real value, and `isEmpty()` reports it.

The rules about the text never look at the disk, so their results are the same whatever exists.
`join(part)` returns this path and `part` with one separator between them, and returns the part alone when the part is absolute or this path is empty; the part may be a `string` or a `Path`.
`parent()` returns the directory one level up, and `null` at a root and for a name with no directory written before it; a path that ends in a separator names the same place as one that does not, so both return the same.
`fileName()` returns the last part of the path, and `""` at a root.
`extension()` returns the text after the last `.` of that last part without the dot, and `stem()` returns the part before it, so for `notes.txt` `stem()` returns `notes` and `extension()` returns `txt`.
A name with no `.`, and one whose only `.` starts it the way a hidden file is named, has no extension and is its own stem.
`isAbsolute()` reports whether the path names a place on its own, which a path starting at a root does, and so does one starting at a drive letter with a separator after it or at a server share; drive-relative text such as `C:foo` does not, because it names a place only against whatever that drive's current directory happens to be.
`normalize()` drops the `.` parts and the repeated separators and resolves every `..` that has a part before it to remove, keeps a `..` with nothing left to remove in a relative path, drops one at a root, returns `.` for a path that turns out to name nowhere in particular, and keeps both separators of a path starting `//`.
Symbolic links are text like any other here, so a normalized path can name a different place than the path it came from.
`absolute(workingDirectory)` resolves a relative path against that directory and normalizes the result, leaving a path that is already absolute only normalized, and `absolute()` resolves it against the directory the program was started in.
`relativeTo(base)` returns the path written relative to `base`, so joining the result to `base` names the same place, and returns `null` for a path that is not under `base`.
Both are normalized and compared part by part as text, so `ab` is not under `a`, a relative path is never under an absolute one nor the other way round, and a path relative to itself is `.`.

The operations that follow do look at the disk, and each of them raises a `FileSystemError` when it could not be carried out.
`metadata()` returns what is at the path as a `Metadata?`, and `null` when nothing is there, so a missing path is a `null` result rather than an error, while every other failure is raised.
`exists()`, `isFile()` and `isDirectory()` are shorthands over it and never throw: they return `false` for a missing path, and also for a failure that is not an absence, since a test that cannot throw has no other value to return.
A `true` result proves nothing about a moment later, so opening a file and catching `NotFound` beats checking first and opening second.
`realPath()` returns the path with every symbolic link resolved, as the operating system resolves it, which needs every part of the path to be there, because a link is resolved by reading it and a `..` written after one names a different place than removing it from the text would.
On Windows it may still return a result for a path whose middle part is missing, since Win32 removes every `..` before it resolves anything.
`linkTarget()` returns the path a symbolic link stores, so a relative target stays relative to the link's directory; it returns `null` for a path that names something other than a link, and raises a `FileSystemError` for a path where nothing is there.
On Windows a junction counts as a link too, and the target is the link's display form, the path as it was written when the link was made.

`readBytes()` and `readText()` return everything the file at the path holds, the first as `bytes`, and `readText` raises a `FileSystemError` for bytes that spell no character.
`writeBytes(content)` and `writeText(content)` write that content, a `byte[]` or a `bytes` for the first, as everything the file holds, creating the file when nothing is there and replacing what was there when something is.
`writeBytesAtomic(content)` and `writeTextAtomic(content)` write to a file beside the path and move that file over the path in one step, so a reader sees the old content or the new and never a half-written file.
`open()` returns the file open for reading, `create()` returns it open for writing and holding nothing, and `append()` returns it open for writing at its end; the last two create the file when nothing is there yet, and a directory is refused when the file is opened rather than at the first read.

A `File` from `@std.fs.file` is both a `Reader` and a `Writer`, so an open file goes wherever either contract goes, and reading or writing one raises an `IoError` whose message names the path.
Writing a file that was opened for reading is refused by the operating system and arrives as that error.
`close()` hands the file back and reports a write that could not be finished, and calling it twice is allowed, with nothing reported the second time.
A file that is dropped without being closed is closed anyway, with any failure kept to itself, so a file that was written and matters is closed with `close`.
`tryLock()` takes the file's lock without waiting and returns whether the file holds it afterwards, which is `true` at once for a file that already holds its lock, and `false` when another open file holds the lock or when the file was removed after it was opened, since a lock on a file no path names keeps nothing out.
The lock lasts until the file is closed, which also happens when the program ends however it ends, and it keeps out only another `tryLock`, so reading and writing the file are not affected.
Reading, writing, flushing or locking a file after `close` is a mistake in the program, and it panics.

`entries()` returns what is directly inside the directory, in ascending byte order of name, with the entries naming the directory itself and its parent left out.
The directory is read to its end before anything is returned, so a walk visits the same names in the same order however the file system enumerates them.
`walk(followLinks)` returns every entry under the directory, depth first, each directory's entries in the order `entries` returns them.
A symbolic link is reported and not descended into, which is what `followLinks` defaulting to `false` means, and a `followLinks` of `true` descends into a link that names a directory while stepping over a place the walk has already been, so a cycle cannot trap the walk either way.
Both return an `Iterable<Entry>`, which a `for`-in loop walks, and an `Entry` from `@std.fs.entry` is a struct holding the entry's `path`, its own `name` with no directory written in front of it, and its `kind`, which is `Symlink` for a link rather than what the link points to.
`walker(followLinks)` returns a `Walker` from `@std.fs.entry`, whose `next()` returns the entries `walk` returns, in the same order, and `null` once there are none left.
A walker reads a directory only when it gets there, and reads all of it before it goes into any directory under it, so no directory stays open while a deeper one is read.
`skip()` leaves out everything under the entry `next()` returned last, so that directory is never read, and after an entry with nothing under it, such as a file, it changes nothing.
Calling `skip()` before `next()` has returned an entry, or after it has returned `null`, is a mistake in the program, and it panics.
`next()` raises a `FileSystemError` for a directory it could not read, and that ends the walk, so every later call returns `null`.

`createDirectories()` creates the directory and every missing directory above it, leaving a path that is already a directory alone.
`createDirectoryExclusive()` creates them the same way and returns whether this call is the one that created the directory itself, so two programs racing for the same name get different results and the one that gets `true` may treat the directory as its own.
`moveTo(destination, replace = false)` moves what is at the path in one step and returns whether this call is the one that moved it, which is `false` when something was already at the destination; both paths have to be on the same volume, because a move across volumes would have to copy.
Some file systems, such as overlay and network file systems, cannot refuse an existing destination as part of the move itself, and neither can Linux kernels older than 3.15.
On those, a move without `replace` checks the destination first and then moves, so two programs moving onto the same name at the same moment can both get `true`.
With `replace` set to `true` it moves over a file already at the destination in the same one step, so it returns `true` or raises a `FileSystemError`, as it does on Windows while another program has that file open.
`copyTo(destination)` copies the file at the path, replacing whatever was at the destination, and the copy carries the source's permissions and never its set-user-id, set-group-id or sticky bits, because a copy hands over permissions and not privileges.
`permissions()` returns who may read, write and execute what is at the path, and `setPermissions(permissions)` gives it new ones; both follow a symbolic link, and a change never sets a set-user-id, set-group-id or sticky bit.
`Permissions` from `@std.fs.permissions` is a struct holding an `Access` for the file's `owner`, for the users of its `group` and for all `others`, and an `Access` is a struct of three `bool` fields, `read`, `write` and `execute`; to execute a directory is to open what is inside it.
Windows records only whether a file is read-only, so there `permissions()` reports that everyone may read and execute anything and may write anything that is not read-only, and `setPermissions` makes a file read-only exactly when its `owner` may not write it.
`removeFile()` removes one file and raises a `FileSystemError` for a directory, and `removeDirectory()` removes one directory, which has to be empty already, so removing what a directory contains always takes a call of its own.
`removeRecursively()` is that call: it removes everything under the path, then the path itself, and stops at the first thing it could not remove.
A symbolic link is removed as the link it is and never followed, so a link into a tree does not take the removal with it.

`Metadata` from `@std.fs.metadata` is a struct holding `kind`, `length` in bytes, which is `0` for a directory, and `modifiedMillis`, when the content last changed in milliseconds since 1970-01-01 UTC.
Its `kind` is an `EntryKind`, one of `File`, `Directory`, `Symlink` and `Other`, the last being something the system keeps that is none of the first three, such as a device or a socket.
A result that followed a symbolic link describes what the link points to, so its kind is never `Symlink`.
On Linux, reading file metadata needs kernel 4.11 or newer.
On an older kernel, the calls that read it, such as `metadata()`, `linkTarget()`, `copyTo()` and `tryLock()`, raise a `FileSystemError` of kind `Other`, and `exists()`, `isFile()` and `isDirectory()` return `false`.

`TemporaryDirectory` and `TemporaryFile` from `@std.fs.temporary` each guard something that exists for as long as the value does.
`TemporaryDirectory.create(prefix)` and `TemporaryFile.create(prefix)` make one under the directory this system keeps work in progress in, which is what `TMP`, `TEMP` or `TMPDIR` names, and `/tmp` on a system other than Windows whose environment names none.
The name is one no other call returns and starts with `prefix`, so the name of something left behind by a program that crashed identifies what made it.
`path()` returns where it is, and the value's destructor removes it, a directory with everything in it, discarding any failure.
`keep()` dismisses that removal and returns the path, which the caller then owns.

A `FileSystemError` from `@std.fs.error` carries the `path` the operation could not finish on, a `kind`, a `nativeError` and a `reason`.
The kinds are `NotFound` for nothing at the path or a missing directory written above it, `PermissionDenied` for an operation refused, `AlreadyExists` for something already there that the operation will not replace, `NotADirectory` for a path written above this one that names a file, `IsADirectory` for a directory named where a file was expected, `DirectoryNotEmpty` for a directory that still holds entries, `NoSpace` for a file system with no room left or a quota used up, and `Other` for anything else the system reported, whose own number the message carries.
The set is closed, so a program that handles every member handles everything the module reports.
Windows has no error number that means a directory was named, so a directory named where a file was expected reports `PermissionDenied` there and `IsADirectory` everywhere else.
`nativeError` is the number the system itself reported, which a program logs rather than acts on: on Windows a failure a file-system call reported carries a Win32 number and one a stream call reported carries the C library's `errno`, because that is what each of them reports, while every other platform carries `errno` throughout.
It is `0` where no call to the system was made.
`reason` is the part of the message that states why the operation did not happen, such as `nothing is there`, for a program that shows the path in its own words.

```ens
import @std.fs;
import FileSystemError from @std.fs.error;
import Path from @std.fs;

saveReport(Path folder, string text) throws FileSystemError {
    try folder.createDirectories();
    try folder.join("report.txt").writeTextAtomic(text);
}

listing(Path folder) throws FileSystemError {
    for (let found in try folder.entries()) {
        print("{found.name} is a {found.kind}");
    }
}
```

---

`@std.environment` describes what surrounds the running program.
`environment.arguments()` returns the arguments the program was started with as a `string[]`, not counting the program's own name, which every system passes as the first of them.
The program's own identity is `environment.executablePath()`, which returns the path of the running program itself, or `null` when the operating system would not report it.
`environment.currentDirectory()` returns the directory the program was started in, and `.` when the operating system would not report it; it is read and never written, so to work somewhere else, a program joins this onto the paths it uses.
`environment.platform()` returns the system the program was built for as a `Platform` from `@std.platform`, one of `Windows`, `Linux` and `MacOS`, which was fixed when the program was compiled and cannot change while it runs.
`Linux` is also the result on every other system the compiler can target, whose behavior it treats as Linux's.

`Environment` from the same module is a set of variables, matched by name the way one system matches them: on Windows two names that differ only in ASCII case are one variable, and everywhere else they are two.
The matching is a rule about the values rather than about the machine, so a set built for any system behaves the same on every system, and `platform()` on the set returns the system whose rule it uses.
`Environment.current()` returns the variables this process inherited, as a snapshot: changing it changes nothing outside the program, and a child is given the variables it should see at the moment it is started.
`Environment.empty(platform)` returns a set with no variables at all, matched by that platform's rule.
`get(name)` returns a variable's value or `null`, `set(name, value)` gives it a value, `remove(name)` takes it out and returns whether it was there, `contains(name)` reports whether the set holds it, `length()` and `isEmpty()` measure the set, `names()` returns the names in ascending byte order, and `copy()` returns a second set holding the same variables under the same rule.
A name is kept as it was written, so a Windows set holding `PATH` that is given `path` reports `path` from then on.
An empty name, and one holding `=`, names no variable a child could be given, so writing one stops the program the way an index out of range does.

```ens
import @std.environment;
import Environment from @std.environment;

describeRun() {
    for (let argument in environment.arguments()) {
        print(argument);
    }
    let variables = Environment.current();
    print(variables.get("PATH") ?? "no PATH");
}
```

---

`@std.process` starts other programs.
No command interpreter takes part unless one is asked for by name, so every argument reaches the program exactly as written, whatever spaces or quotes it holds, and nothing is expanded on the way.
`process.run(program, arguments, workingDirectory, environment, captureOutput)` runs a program, waits for it to finish, and returns a `CommandOutput`.
Everything after the program is optional: the arguments default to none, the working directory and the environment to this process's own, and `captureOutput` to `false`.
The child reads and writes this process's own three streams, so what it writes appears where it would have appeared as it is written; with `captureOutput` its two output streams are collected into the result instead, and only its input stays this process's own.
A captured run returns once both of those streams have ended, so a grandchild that inherited them delays the return until it closes them too.
The `environment` given is exactly what the child sees rather than something laid over what it would have inherited, so patching means `Environment.current()` and `set`, and a clean slate means `Environment.empty(platform)` and `set`.

A `program` with no separator in it is looked for in the `PATH` directories of the environment the child will receive, never in the current directory, and on Windows as `name.exe` unless the name already has an extension.
So `run(Path("build"))` never finds a `build.cmd`, while `run(Path("build.cmd"))` does find one where a `PATH` directory holds it, since a name that has an extension is looked for exactly as it is written.
Windows then runs that script through its own command interpreter, although the call to `run` named none, so a program that runs a script calls `runShell`.
A `program` with a separator in it names one place, read against this process's directory when it is relative.
On a system other than Windows, every `PATH` directory holding a file of that name is offered to the system in turn, and one the system will not run for lack of permission is passed over for the next, so that permission failure is reported only when no later candidate runs.
On Windows the first file found is the only candidate, because nothing there marks one file of that name as the one to run in preference to another.

`process.runShell(commandLine, workingDirectory, environment, captureOutput)` runs one command line through the operating system's command interpreter, with everything that implies: the shell splits, expands and interprets the line.
An argument built from something the program read belongs in `run`, which interprets nothing.
The interpreter is `/bin/sh` with the line after `-c`, and on Windows the program `%ComSpec%` names, or `cmd.exe` found on `PATH` when it is not set, with the line after `/S /C` so `cmd` runs it as written.
Failing to start the interpreter is an error; a line that ran and failed is a status to inspect.

A `CommandOutput` is a struct holding the `status` the program ended with and the `stdout` and `stderr` it wrote, which are empty text unless the run captured them.
A captured byte that spells no character becomes the replacement character, so both texts are text like any other.
An `ExitStatus` is a struct holding a `code` and a `signal`, of which exactly one means something: a program that exited has its code and a signal of `0`, and one a signal ended has that signal and a code of `0`.
`succeeded()` reports whether the program exited by itself with a code of `0`.
Windows has no signals, so a child that crashed there reports its own status as a negative code, while one ended by this library's `kill()` reports signal `9` and code `0`, the numbers a POSIX system reports for the same act, so one check distinguishes a landed kill from an exit on every platform.

`process.spawn(program, arguments, workingDirectory, environment, mergeOutput)` starts a program and hands it back as a `ChildProcess` while it is still running, with a pipe on each of its three streams, so this process writes what the child reads and reads what it writes.
`stdout()` and `stderr()` return a `BufferedReader` over one of the child's output streams, `input()` returns a `Writer` onto its input, and `closeInput()` ends the child's input.
Each of the three returns the same value every call, as `io.in()` does and for the same reason.
Draining one of the output streams to its end while the child fills the other can leave both sides waiting, since a full pipe stops the child, so a program reads the stream the child actually writes to, or captures through `run`.
With `mergeOutput`, which defaults to `false`, the child writes its standard error into the same pipe as its standard output, so `stdout()` reads both streams in the order the child wrote them, and `stderr()` returns a reader that has already ended.
A merged child leaves no second stream for this process to drain, which is the shape for a program whose two streams are one account of what it is doing, such as a build tool whose lines are shown as they arrive.
`waitForOutput(timeoutMillis)` reports whether reading can go ahead without waiting for the child: `true` when either stream has bytes or has ended, and `false` when that many milliseconds went by with neither, where a bound of `0` checks this moment alone.
Bytes that `stdout()` or `stderr()` has already taken from its pipe count as well, so a second line that arrived together with the first is reported without asking the system.
Reading itself waits as long as the child takes, so a program that can wait never needs to call `waitForOutput`.
`wait()` returns the exit status, waiting for the child to finish first and reading and throwing away output nobody read meanwhile, because a child whose output nobody reads is stopped by a full pipe.
`wait(timeoutMillis)` is the same call bounded, returning `null` once that many milliseconds have passed with the child still running.
Either one returns the same status however often it is called.
`kill()` ends the child now and returns without waiting for it, and a child that had already ended is left as it is and keeps the code it ended with.
A `ChildProcess` that is dropped lets the child go: it keeps running with nothing reading it, and on a POSIX system it stays a finished child nothing has collected until this program exits, which is why ending a child is explicit through `kill` or `wait`.

A write to a pipe whose reader has gone, which includes a write to a child that has closed its input and any write after `closeInput`, raises an `IoError` of kind `Closed`.
It never ends this program instead: the signal a system would otherwise deliver for such a write is refused before any of the program's own code runs, since ending there would run not one destructor.

Failing to start a program raises a `ProcessError`, which carries the `program` as the caller wrote it, a `kind`, and a `nativeError`.
The kinds are `NotFound` for a name no `PATH` directory holds a program under and for nothing at the path written, `PermissionDenied` for a file the system refused to run, and `Other` for anything else the system reported.
`nativeError` is the system's own number, a Win32 number on Windows and an `errno` everywhere else, and `0` where no call to the system was made, as it is for that name no `PATH` directory holds.
A program that started and then failed is not a failure this module reports but a `status` to inspect.

```ens
import Path from @std.fs;
import IoError from @std.io.streams;
import @std.process;
import ProcessError from @std.process;

gitVersion() -> string throws ProcessError {
    let finished = try process.run(Path("git"), ["--version"], captureOutput: true);
    if (!finished.status.succeeded()) {
        return "";
    }
    return finished.stdout.trim();
}

relay(string url) -> int throws ProcessError, IoError {
    let child = try process.spawn(Path("git"), ["clone", url]);
    string? line = try child.stdout().readLine();
    while (line != null) {
        print(line);
        line = try child.stdout().readLine();
    }
    let status = try child.wait();
    return status.code;
}
```

---

`@std.json` reads JSON text into a tree of values and writes a tree back as text.
Every value is a `Json`, a sealed class whose six final subclasses are the kinds JSON has: `JsonNull`, `JsonBool`, `JsonNumber`, `JsonString`, `JsonArray` and `JsonObject`.
A program that walks a value it knows nothing about switches over it with an `is` arm per kind, which the compiler checks is exhaustive, and a program that knows the shape it expects probes with `isNull()`, `asBool()`, `asLong()`, `asDouble()`, `asString()`, `asArray()` and `asObject()`, each returning the content when the value is of that kind and `null` otherwise, so `release.get("tag_name")?.asString()` is the tag as a `string?` whether or not the member is there.
`Json.parse(text)` returns the value the text holds, which has to be JSON as RFC 8259 defines it: no comments, no trailing commas, no single quotes, no leading zeros, no `NaN` or infinity, whitespace allowed around every token, and a byte order mark allowed in front.
Anything else raises a `JsonError`, whose `offset`, `line` and `column` say where the text stops being JSON, the column counted in bytes from the start of its line, and whose message says what was found there and what JSON allows in its place.
An object that names the same key twice is refused, naming both places; `Json.parse(text, true)` keeps the last value instead.
Nesting deeper than 512 levels is refused.

A `JsonNumber` keeps the text the number was written with, so nothing is rounded between reading a number and writing it again and a number longer than any numeric type is carried through untouched.
`asLong()` returns the number only when it is written as a whole number a `long` holds, so `1.0` and `1e2` return `null`, and `asDouble()` returns the nearest `double`, or `null` outside the range a `double` holds.
`JsonNumber.of(value)` makes one from a `long` or a `double`, written as the value prints, and stops the program for a `NaN` or an infinity, which JSON has no spelling for.
A `JsonArray` holds values in order, with `get`, `set`, `push`, `removeAt`, `clear`, `length` and `isEmpty`, and is walked with `for (let element in array)`.
A `JsonObject` holds members by key in the order they were added, which is the order they are written in and walked in as `Entry<string, Json>` values; `set` on a key already there replaces its value and keeps its place, `get(key)` returns `null` for a key that is not there, and `keys()` returns the keys in order.
Two values are equal, and hash alike, when they are the same kind with equal content, an object's key order left aside, so a parsed value compares with one built by hand.

`toString()` writes a value as JSON on one line in the form a struct prints as, `", "` between elements and members and `": "` after a key, with `"`, `\` and control characters escaped the same way, so a `JsonObject` and a struct holding the same fields print the same text.
`toString(indent)` writes it over several lines, every element and member on a line of its own and indented by one `indent` per level of nesting, with an empty array or object as `[]` or `{}`.

```ens
import Json from @std.json;
import JsonError from @std.json;

newestTag(string releases) -> string? throws JsonError {
    let list = (try Json.parse(releases)).asArray();
    if (list == null || list.isEmpty()) {
        return null;
    }
    return list.get(0).asObject()?.get("tag_name")?.asString();
}
```

---

`@std.tasks` starts tasks, waits for them and stops them, and every task belongs to a scope.
`tasks.scope(body)` makes a `TaskScope`, runs `body` with it on the calling task, and returns what the body returns once the body has returned and every task started in the scope has finished, so no task outlives the block that started it.
`scope.start(body)` starts `body` on another task at once and returns a `Task` without waiting, so two calls in a row run two tasks at the same time.
The type is `Task<F>` where `F` is the function type the task runs: a body returning a `Module` and throwing `ParseError` or `IoError` gives a `Task<(() -> Module throws ParseError, IoError)>`, a body returning a value and throwing nothing a `Task<() -> Module>`, and a body returning nothing a `Task<() -> void>`; a local holding one is written with `let`, and the type is inferred.
`task.await()` waits for the task and returns what its body returned, or throws what its body threw, so a call of it is written with `try` exactly when the body can throw.
A `Task` is shareable, so a task may await one of its siblings, and a value that is not shareable is taken once, a second `await()` aborting the program, while a shareable value is returned to every awaiter.
`scope.launch(body)` starts a body that returns nothing and cannot throw, and returns nothing: it is the form for work nobody waits for, such as serving one connection, and a body that could throw is refused with the advice the lambda rules give, to move the work into a function with a `catch` clause of its own.
A task started with `start` is meant to be awaited, so one that threw and was never awaited aborts the program when its scope ends, naming the line that started it, because an error nobody handled is unhandled.
`scope.startDedicated(priority, body)` is `start` on an operating-system thread of the task's own, for a loop that must not share one, such as audio or rendering, under the same rules for what the body may capture.
A `TaskScope` is also made with `new TaskScope()` and kept in a field, for tasks that live as long as an object does; its destructor cancels every task in it and waits for them all, and since no task can hold a reference to a writable object such as its owner, the wait is never for itself.
Starting a task in a scope that has ended aborts the program.

```ens
import @std.tasks;

loadBoth(Path first, Path second) -> Module[] throws ParseError, IoError {
    return try tasks.scope((scope) -> {
        let parsing = scope.start(() -> try parse(first));
        let other = scope.start(() -> try parse(second));
        return [try parsing.await(), try other.await()];
    });
}
```

A task stops when it is asked to and when it chooses to: cancellation is cooperative.
`scope.cancel()` marks every task in the scope cancelled, `task.cancel()` marks one task and everything it started, and `scope.cancelAfter(millis)` marks the scope after that many milliseconds, which is how a deadline is written.
The first task in a scope to throw marks the scope cancelled as well, so the others stop wasting work, while the scope still waits for every one of them before the error reaches whoever awaits it.
A task reads `tasks.isCancelled()` to learn that it was asked to stop, and `tasks.checkCancelled()` throws `Cancelled` when it was, so a loop writes `try tasks.checkCancelled();` once per iteration, and its body's throws list, and so its task's type, then say that the task can be cut short.
A call that waits, on a stream, a channel, a lock or a task, ends early in a cancelled task by throwing the error it declares anyway, an `IoError` for a read, with a `Cancelled` as its cause.
A task whose body never checks runs to its end, and its type shows that it cannot be cut short.
Two tasks that await each other, and a task that locks a `Mutex` it already holds, could wait for ever, so each aborts the program naming the lines that started the tasks instead.
`tasks.sleep(millis)` blocks the calling task until the monotonic clock has advanced by that many milliseconds; the count is a floor and never a ceiling, so nothing bounds how long a loaded machine leaves the task waiting, and a count of zero or less returns without waiting at all.

`Channel<T>` from `@std.tasks` carries shareable values from the tasks that send to the tasks that receive.
`new Channel<T>(capacity)` is the receiving end: `receive()` waits for the next value and returns it, or `null` once the channel is closed and empty, `receive(timeoutMillis)` returns `null` after that many milliseconds as well, with `isClosed()` telling the two apart, and a `for` loop walks a channel one value at a time until it is closed and empty, so two loops on two tasks share the work between them.
`channel.sender()` returns a `Sender<T>`, whose `send(value)` puts a value in and waits while the channel already holds `capacity` values; a capacity of zero makes every send wait for a receive.
The channel closes when the last `Sender` is released, so a producer closes nothing and a loop over the channel ends exactly when every producer has finished; a sender kept in a field keeps the channel open for as long as the field does.
A send on a channel whose receiving end has been released throws `ChannelClosed`, since the value would never arrive.
There is no way to wait on two channels at once; two sources merge by sending into one channel, each through a sender of its own, and a wait with a time limit is `receive(timeoutMillis)`.

```ens
const lines = new Channel<Line>(64);
for (let path in paths) {
    scope.launch(() -> read(path, lines.sender()));   // read(Path, Sender<Line>) holds one sender
}
for (let line in lines) {                             // ends when the last producer has finished
    consume(line);
}
```

`Mutex<T>` from `@std.tasks` holds one value that several tasks change in turn.
`new Mutex(value)` takes the value in, and `mutex.lock(body)` runs `body` with the value while no other task has it, waiting for its turn, and returns what the body returns.
The body is a `shared` function and its result must be shareable, so the value inside cannot leave the lock through a capture or a return, while a shareable part of it, such as an entry of a map of `shared` objects, can.
`Atomic<T>` from `@std.tasks` holds one shareable value that tasks read and replace without a lock: `read()`, `write(value)`, `compareAndSet(expected, replacement) -> bool`, and for numbers `increment()`, `decrement()` and `add(amount)`, each returning the value after the change.
An `Atomic` of a `shared` class replaces a whole table under its readers, which see the old value or the new and never a mix of the two.

`@std.parallel` runs one body over many pieces of data at once and returns when every piece is done, on the task that called it, which it parks for the whole call.
`parallel.map(items, body)` runs the body once per item and returns an array of the results in the items' order, however the pieces finished; `parallel.each(items, body)` is the same for a body that returns nothing; `parallel.range(start, end, body)` runs the body once per index of a half-open range.
A body that can throw makes the call throw the same types, the first failure cancels the pieces still running, and the call throws once every piece has ended.
Each body is a `shared` function, so it captures shareable values only, and the items are handed to other tasks, so their type must be shareable, as the `Path` values of a file walk are, while what a body returns may be anything, being a result.
`parallel.split(array, parts, body)` cuts an array into `parts` disjoint `Slice<T>` views, runs the body once per slice on another task, and gives the array back whole once every body has returned; a slice is read and written through `get(index)` and `set(index, value)`, with `length()` and `start`, the index of its first element in the array.
Only the calling task could write the array, and it is parked inside the call, so a body writes its slice with no copy and no lock, and the slice is a parameter that is not shareable, so no body can keep it past the call.
`parallel.split(array, parts, reading(other), body)` lends every body a whole read-only view of a second array beside its slice, for data each piece reads while writing its own part; naming the array being written as a read view aborts the program, since a piece would read what another writes.
An element type that is a writable class is refused by the bound, because two slices could reach one object through two elements; an array of structs, or of `shared` objects, is what `split` is for.

```ens
import @std.parallel;

let modules = try parallel.map(files, (file) -> try parse(file));
parallel.split(pixels, tiles, reading(depth), (tile, depthView) -> shade(tile, depthView, scene));
```

---

Every value has a `hash()` method returning a `long`. Value types (primitives, enums, strings, structs) and arrays hash by their contents, so equal values hash equally; classes hash by identity, matching how `==` compares them.
An optional hashes as its payload does while it is present and as one fixed value once it is absent, so every absent value hashes equally whatever its type.
A class or a struct can declare its own `hash() -> long` to control its hashing, paired with `equals(T other) -> bool`, a method taking a single parameter of the declaring type `T` itself, to control equality.
A method named `hash` must have exactly that signature, and neither `hash` nor `equals` can be `throws`, because the language takes a value's hash and compares two values where there is no room for a `try`; `equals` must return `bool`.
When a class declares such an `equals`, `==` and `!=` on that class compare by content, running an identity and null check first and then `equals`, rather than by reference identity; when a struct declares one, `==` and `!=` call it instead of comparing the fields.
Both `hash` and `equals` are written with `override`, since they replace behavior the language provides: a class's identity hash and equality, a struct's content hash and memberwise equality.
The two are a matched pair: a type that declares one must declare the other, so equal values always hash equally.
A declared `hash` defines the hashing of its type everywhere the value appears, including as a field of an enclosing struct, as an array element, and through a type parameter; a declared `equals` defines `==` the same way.
A type parameter needs no bound to be hashed, since every type but an external handle, a function value, and a floating-point number has `hash()`, which is why `Map` and `Set` name their key types without one; an instantiation at one of those is an error naming the type.
A struct field with no hash for any type argument, such as a `double` field or one of type `(T) -> int`, makes `hash()` on that struct an error in the generic body itself, and no instantiation reports it again.
Because the language calls `hash` and `equals` wherever the type is used, both follow their type's visibility when unmarked and may not be marked less visible than the type itself.

For a class, which implementation runs is determined by the value's type at run time, not by the type written in the source.
An object of a class that declares `hash` and `equals` keeps them when it is held in a variable, field, array or collection typed as a base class or as an interface, so two such objects that are equal as their own class stay equal and hash alike where the code holding them names only the base type.
Because a class's `equals` takes its own class, two objects compare by content only when their run-time types are the same one; objects of different run-time types are never equal, even when one class inherits the other's `equals`.
A class that declares neither method, a base class whose subclasses declare them included, keeps identity equality and the identity hash for objects of exactly that class.

Three contracts describe what a container is, and the collection modules build them on hashing and iteration.
`Iterator<T>` from `@std.collections.iterator` declares `next() -> T?`, `Iterable<T>` in the same module declares `makeIterator() -> Iterator<T>`, which is what a `for`-in loop calls, and `Collection<T>` from `@std.collections.collection` extends `Iterable<T>` with `length()`, `isEmpty()` and `contains(value)`.
All three are read-only, so what changes a container is declared on the container itself.
`List`, `Set`, `Deque` and `PriorityQueue` are `Collection<T>`s, while `Map` and `SortedMap` are `Iterable<Entry<K, V>>`s.
Every container is a reference type, as a class is, and each has a `copy()` that returns a second container holding the same values, which leaves the values themselves shared between the two.
A container aborts the program rather than returning a value it does not have, so an index outside the container, and `pop`, `popFront`, `popBack`, `first`, `last`, `peek`, `firstKey` or `lastKey` on an empty one, are mistakes in the caller rather than conditions to recover from.
`frozen()` on a list, a map, a set or an array returns a `FrozenList<T>`, a `FrozenMap<K, V>` or a `FrozenSet<T>` holding the same values, which nothing can write afterwards, so it is shareable when its values are and may cross to another task.
When nothing else refers to the container, its storage moves into the frozen one and nothing is copied, which is the case for a container built in a local and frozen at its last use; otherwise the values are copied, and `--explain-arc` names each freeze that copied.

- `List<T>` from `@std.collections.list` is a growable sequence holding its values in the order they were put in: `push(value)`, `pushAll(values)`, `pop()` taking the last value off and returning it, `get(index)`, `set(index, value)`, `first()`, `last()`, `insert(index, value)`, `removeAt(index)`, `remove(value)` removing the first value equal to it, `clear()`, `reserve(capacity)`, `copy()`, and `toArray()` returning a fresh right-sized `T[]` holding the current contents.
  `indexOf(value)` and `indexWhere(test)` return where the first match sits, or `-1` when there is none, and `removeWhere(test)` removes every value a test accepts in one pass and returns how many went.
  `sort(order)` puts the values in the order a comparison describes, which returns a negative value when its first argument sorts first, and `sort()` with no argument uses the natural order.
  A list of numbers, characters, or strings sorts with no comparison given, and any other element type sorts that way once it implements `Comparable`.
  `sorted` returns a new list rather than reordering this one, and `reverse` and `reversed` are the same pair for turning the order around.
  Whether values the order calls equal keep the order they arrived in is not guaranteed.
  `List.of(values)` returns a list holding an array's values, and `List.withCapacity(capacity)` an empty list with room for that many.
  Iterating a list yields its values in insertion order.
- `Map<K, V>` from `@std.collections.map` finds values by key: `set(key, value)` inserts or overwrites, `get(key)` returns `V?` and `null` when the key is absent, and `getOrInsert(key, make)` returns the value under the key, storing what `make` builds when there is none, plus `contains(key)`, `remove(key)`, `removeWhere(test)`, `clear()`, `length()`, `isEmpty()`, `copy()`, `Map.withCapacity(capacity)`, and `keys()` and `values()`, which are views onto the live map.
  Iterating a map yields `Entry<K, V>` entries from `@std.collections.entry`, each a struct with a `key` and a `value` field, while the two views yield the keys and the values themselves.
  For a map whose values are themselves nullable, a stored null and an absent key are distinguished by the two levels of the value `get` returns: the outer null means the key is absent.
  The order a walk visits a map's entries in is not specified and may change between releases, so a program that needs an order sorts what it read or holds a `SortedMap` instead.
  Adding or removing entries in a map or a set while it, or one of a map's views, is being walked aborts the program; overwriting the value under a key the map already holds does not.
  Make the change after the walk, or remove with `removeWhere`.
- `Set<T>` from `@std.collections.set` stores each value once: `add(value)` returns whether the value was new, plus `contains(value)`, `remove(value)`, `removeWhere(test)`, `clear()`, `length()`, `isEmpty()`, `copy()` and `toArray()`, and `union`, `intersection`, `difference` and `isSubsetOf` over a second set.
  `Set.of(values)` and `Set.withCapacity(capacity)` return a set the way the list statics return a list.
  Iterating a set yields its values, in an order that is not specified either.
- `Deque<T>` from `@std.collections.deque` grows and shrinks at either end: `pushFront(value)` and `pushBack(value)`, `popFront()` and `popBack()` removing and returning a value, `first()`, `last()`, and `get(index)` counting from the front, plus `clear()`, `reserve(capacity)`, `copy()`, `toArray()` and `Deque.withCapacity(capacity)`.
  There is no `set(index, value)`, because writing through a position is a list's operation.
  Iterating a deque yields its values from the front to the back.
- `PriorityQueue<T>` from `@std.collections.priorityqueue` takes values out smallest first: `push(value)`, `pop()` removing and returning the smallest value, and `peek()` reading it without removing it, plus `clear()` and `copy()`.
  "Smallest" is what the comparison given to the constructor defines, or the natural order of the element type for the constructor that takes nothing.
  Numbers, characters, and strings have that order already, and any other type has it once it implements `Comparable`.
  Iterating a priority queue yields every value once, in no particular order.
- `SortedMap<K, V>` from `@std.collections.sortedmap` has the operations of `Map` apart from `withCapacity`, plus `firstKey()` and `lastKey()`, and walks its entries and its views in key order.
  That order is the natural order of the key type, under the same rule as a priority queue's, or the comparison given to the constructor.
  Adding or removing entries while a sorted map or one of its views is being walked aborts the program, as it does for a `Map`.
- `FrozenList<T>`, `FrozenMap<K, V>` and `FrozenSet<T>` from `@std.collections.frozen` hold what they were frozen with and offer the reads of their counterparts, `get`, `length()`, `isEmpty()`, `contains`, `indexOf`, `first()`, `last()`, `keys()`, `values()` and iteration in the same order, and none of `set`, `push`, `remove` or `clear`.
  `toList()`, `toMap()`, `toSet()` and `toArray()` return a writable copy, and `FrozenList.of(values)` freezes an array or a list in one step.
  A frozen collection compares and hashes by content, and it can be a key, since its content cannot change while it sits in the table.

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

Keys are matched with `==` and bucketed with `hash()`: strings by contents, value types by value, and classes by identity, unless a key's run-time class declares `equals` with its paired `hash`, in which case that key matches by content.
A map or set keyed by a base class or an interface therefore finds the entry a derived key stored, because the key's own class determines how it is matched and bucketed; two keys of different run-time classes never match.
Struct keys are supported and match by content: their fields compare with `==` and hash by content, so a key rebuilt from equal field values finds the entry stored under the original.
A struct key that declares its own `equals` and `hash` is matched and bucketed by that pair instead, so a field the pair ignores does not change which entry a key finds.
An array cannot be a key: it compares and hashes by content, and its content can change while it sits in the table, so the entry would silently become unfindable.
A writable collection cannot be a key for the same reason, nor can a struct whose fields, at any depth, hold an array or a writable collection; the error names the field, while a frozen collection can be a key, since its content cannot change.
A struct holding one is still accepted as a key when it declares its own `equals` and `hash`, because that pair is what determines which entry a key finds, and a field it ignores cannot move one.
A `SortedMap` accepts a struct whose fields hold one whether or not it declares that pair, since the `compareTo` it declares is what puts its keys in order.
An external handle or a function value cannot be a key either, because neither has a hash to bucket by.
Nor can a `float` or a `double`, nor a struct whose fields hold one at any depth unless it declares its own `equals` and `hash`.
A `NaN` is equal to no value, so nothing could find it again, and negative zero is equal to zero but hashes differently, so one key would become two entries.
A `SortedMap` keys numbers by their order instead, which needs no hash.
The pair such a struct declares compares each of those fields with `compareTo` and hashes it with `toCanonicalBits`, so a `NaN` matches a `NaN` and the two zeros stay two keys, exactly as that order puts them.

What text can do is declared on `string` itself, so every member below is called on the text and needs no import.
`byteAt(index)` returns one of the UTF-8 bytes, and an index outside the text aborts the program.
`isEmpty()`, `contains(needle)`, `indexOf(needle)`, `indexOf(needle, from)` and `lastIndexOf(needle)` search by exact bytes, returning the byte offset of an occurrence or `-1`; an empty needle is found at offset `0`, and looking back, at the end.
`startsWith(prefix)` and `endsWith(suffix)` report whether the first or last bytes are exactly that part; an empty part always matches, and a part longer than the text never does.
`substring(start, end)` and `substring(start)` return the bytes of a half-open byte range as new text; a range outside the text, or one that cuts through the middle of a character, aborts the program.
`trim()`, `trimStart()` and `trimEnd()` drop the whitespace at both ends or at one, where whitespace is a space or one of the ASCII layout controls: tab, line feed, vertical tab, form feed, and carriage return.
`replace(needle, replacement)`, `repeat(times)`, `padStart(width, filler)` and `padEnd(width, filler)` build new text; an empty needle occurs nowhere to replace and gives the text back as it was, a negative `times` aborts, text already `width` bytes wide is returned as it is, and a filler of several bytes is only ever added whole.
`split(separator)` returns the parts between the occurrences of the separator, so two neighboring separators give an empty part and text holding none gives one part, and an empty separator does the same.
`lines()` splits on `\n` and drops a carriage return before it, so text written with either line ending gives the same lines; text ending in a newline has a last, empty line.
`toLowerAscii()`, `toUpperAscii()` and `equalsIgnoreCaseAscii(other)` convert or compare the ASCII letters only and keep every other byte as it is; the names include `Ascii` because the full Unicode rules are a different operation.
`compareTo(other)` orders text by its bytes, which is code point order.
`toBytes()` returns the UTF-8 bytes as `bytes` sharing the text's storage, while `chars()` and `bytes()` walk the characters or the bytes without copying; text does not iterate on its own, so a walk always names which view it reads.
`string.fromBytes(data)` builds text from UTF-8 bytes, given as `bytes` or as a `byte[]`, and throws `EncodingError` at the offset where the bytes spell no character, `string.fromBytesLossy(data)` writes U+FFFD in place of each such sequence instead, and `string.joined(parts, separator)` puts the separator between neighboring parts and nothing before the first or after the last.

```ens
string[] fields = "name,age,city".split(",");
print(string.joined(fields, " | "));
print("7".padStart(3, '0'));            // "007"
for (let character in "héllo".chars()) { /* ... */ }
```

Every integer type declares `toString(radix)` beside the no-argument `toString()` the language provides, writing the value in a base from 2 through 36 with lowercase digits; a radix outside that range aborts the program, and a negative value keeps its sign.
There is no width argument, because `padStart` already composes with it: `count.toString(16).padStart(4, '0')`.

The `@std.text.parse` module parses values out of text.
`parseLong(text)`, `parseLong(text, radix)`, `parseInt(text)`, `parseDouble(text)` and `parseBool(text)` each return the value the text spells, or `null` when it spells anything else, which includes a number the type cannot hold.
Surrounding whitespace, a radix prefix such as `0x`, and digit grouping are all refused rather than guessed at, so text a program means to accept in those shapes is trimmed or rewritten before it is parsed.
An integer is at most one `-` or `+` and then digits; in another base the letters stand for the digits above nine, in either case.
A double has the shape a floating-point literal has, without the underscores a literal may group its digits with, and the text a `double` renders as is parsed back to exactly that `double`.
`nearestDouble(text)` is the conversion under it, for text already known to spell a number: it returns the nearest `double` rather than a nullable one, with a magnitude past the range giving an infinity and one below it giving zero or a subnormal.
Both round once, to the nearest value, and a number exactly halfway between two doubles rounds to the one with the even last bit, which is also how the compiler folds a floating-point literal, so a number written in source and the same text read here are one value.
`parseBool` reads `true` and `false` spelled exactly that way and nothing else.
A radix outside 2 through 36 aborts, as it does when writing: the radix is the caller's own, where the text is the data.

```ens
import @std.text.parse;

long? count = parse.parseLong("42");
long? mask = parse.parseLong("ff", 16);         // 255
double? ratio = parse.parseDouble("2.5");
long? refused = parse.parseLong(" 42");        // null, whitespace is not trimmed
```

`StringBuilder` from `@std.text.stringbuilder` accumulates text in a growable buffer, so building a string piece by piece stays linear where repeated `+` on immutable strings would re-copy the whole prefix.
`append(value)` accepts a string, a `char`, an integer, a `double`, or a `bool`; `appendLine(value)` and `appendLine()` add a newline after it; `length()` and `isEmpty()` report what is held so far; `clear()` empties it; `reserve(capacity)` and `StringBuilder.withCapacity(capacity)` reserve room ahead of time; `toString()` returns the accumulated text and leaves the builder usable.
Every append takes text or a value with a text form, so what a builder holds is valid UTF-8 and `toString` needs no check of its own.

```ens
import StringBuilder from @std.text.stringbuilder;

let report = new StringBuilder();
report.append("processed ");
report.append(count);
report.append(", ok: ");
report.append(allPassed);
print(report.toString());
```
