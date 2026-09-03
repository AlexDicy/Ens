# @std.collections

Six containers over a two-level, read-only protocol ladder.
Comparator and predicate parameters are written `(T, T) -> int` and `(T) -> bool` as placeholders for the undecided closure spelling.

## Contracts

```ens
// @std.collections.iterator
// The contracts a container implements to be walked, counted, and searched.

// A walk over values, taken once. `next` answers the value it moved onto, or null when the walk is
// over. Every call after that answers null.
export interface Iterator<T> {
    next() -> T?;
}

// Something `for (let item in ...)` can walk. The loop asks for one iterator and draws values until
// it runs out.
export interface Iterable<T> {
    makeIterator() -> Iterator<T>;
}

// An iterable that knows how many values it holds and can answer whether it holds a given one.
export interface Collection<T> extends Iterable<T> {
    length() -> long;
    isEmpty() -> bool;
    contains(T value) -> bool;
}
```

```ens
// @std.collections.entry
// One key with its value, which is what walking a map yields.
export struct Entry<K, V> {
    export const K key;
    export const V value;
}
```

## List

```ens
// @std.collections.list
// A growable sequence of values, kept in the order they were put in.
export final class List<T> implements Collection<T>, Copyable<List<T>> {
    export constructor();

    // A list that can hold `capacity` values before it has to grow. The length is still zero.
    export static withCapacity(long capacity) -> List<T>;

    // A list holding `values`, in that order.
    export static of(T[] values) -> List<T>;

    export override length() -> long;
    export override isEmpty() -> bool;
    export override contains(T value) -> bool;
    export override makeIterator() -> Iterator<T>;

    // The value at `index`. An index outside the list aborts the program, because reading past the
    // end is a mistake in the caller rather than a condition to recover from.
    export get(long index) -> T;
    export set(long index, T value);

    // The first and last values. Either one aborts the program on an empty list.
    export first() -> T;
    export last() -> T;

    export push(T value);
    export pushAll(Iterable<T> values);

    // Takes the last value off and answers it. Aborts the program on an empty list.
    export pop() -> T;

    export insert(long index, T value);
    export removeAt(long index) -> T;

    // Removes the first value equal to `value`, and answers whether there was one.
    export remove(T value) -> bool;

    // Removes every value the test accepts, in one pass, and answers how many went.
    export removeWhere((T) -> bool test) -> long;

    export clear();
    export reserve(long capacity);

    // Where the first value equal to `value` sits, or -1 when the list holds none.
    export indexOf(T value) -> long;

    // Where the first value the test accepts sits, or -1 when the test accepts none.
    export indexWhere((T) -> bool test) -> long;

    // Puts the values in the order `order` describes: negative when its first argument sorts
    // first, zero when neither does, positive when the second does. Whether values the order calls
    // equal keep the order they arrived in is not promised.
    export sort((T, T) -> int order);
    export sort<T: Comparable<T>>();

    export sorted((T, T) -> int order) -> List<T>;
    export sorted<T: Comparable<T>>() -> List<T>;

    export reverse();
    export reversed() -> List<T>;

    export override copy() -> List<T>;
    export toArray() -> T[];
}
```

## Map

```ens
// @std.collections.map
// Values found by key. The order a walk visits them in is not specified and may change between
// releases, so a program that needs an order sorts, or holds a SortedMap instead.
export final class Map<K, V> implements Iterable<Entry<K, V>>, Copyable<Map<K, V>> {
    export constructor();
    export static withCapacity(long capacity) -> Map<K, V>;

    export length() -> long;
    export isEmpty() -> bool;

    // The value stored under `key`, or null when the map holds no such key. For a map whose values
    // are themselves nullable, a stored null and a missing key are told apart by the two levels:
    // the outer null means the key is absent.
    export get(K key) -> V?;

    export set(K key, V value);
    export contains(K key) -> bool;

    // Removes the key and its value, and answers whether there was one.
    export remove(K key) -> bool;

    // Removes every entry the test accepts, in one pass, and answers how many went.
    export removeWhere((K, V) -> bool test) -> long;

    export clear();

    // The value under `key`, storing what `make` builds when the key is absent. `make` runs only
    // then, and the key is found once rather than once per step.
    export getOrInsert(K key, () -> V make) -> V;

    // Views onto the live map, walked without copying anything. Changing the map while one of these
    // is being walked aborts the program.
    export keys() -> Iterable<K>;
    export values() -> Iterable<V>;

    export override makeIterator() -> Iterator<Entry<K, V>>;
    export override copy() -> Map<K, V>;
}
```

## Set

```ens
// @std.collections.set
// A collection that holds each value once. The order a walk visits them in is not specified.
export final class Set<T> implements Collection<T>, Copyable<Set<T>> {
    export constructor();
    export static withCapacity(long capacity) -> Set<T>;
    export static of(T[] values) -> Set<T>;

    export override length() -> long;
    export override isEmpty() -> bool;
    export override contains(T value) -> bool;
    export override makeIterator() -> Iterator<T>;

    // Adds `value`, and answers whether it was not already there.
    export add(T value) -> bool;
    export remove(T value) -> bool;
    export removeWhere((T) -> bool test) -> long;
    export clear();

    export union(Set<T> other) -> Set<T>;
    export intersection(Set<T> other) -> Set<T>;
    export difference(Set<T> other) -> Set<T>;
    export isSubsetOf(Set<T> other) -> bool;

    export override copy() -> Set<T>;
    export toArray() -> T[];
}
```

## Deque

```ens
// @std.collections.deque
// A sequence that grows and shrinks at either end, keeping values in the order they were put in.
export final class Deque<T> implements Collection<T>, Copyable<Deque<T>> {
    export constructor();
    export static withCapacity(long capacity) -> Deque<T>;

    export override length() -> long;
    export override isEmpty() -> bool;
    export override contains(T value) -> bool;

    // Walks from the front to the back.
    export override makeIterator() -> Iterator<T>;

    export pushFront(T value);
    export pushBack(T value);

    // Take a value off either end. Both abort the program on an empty deque.
    export popFront() -> T;
    export popBack() -> T;

    export first() -> T;
    export last() -> T;

    // The value `index` places from the front. An index outside the deque aborts the program.
    export get(long index) -> T;

    export clear();
    export reserve(long capacity);
    export override copy() -> Deque<T>;
    export toArray() -> T[];
}
```

## PriorityQueue

```ens
// @std.collections.priorityqueue
// Values taken out smallest first, whatever order they were put in. "Smallest" means what the
// natural order of T says, or what the order given at construction says.
export final class PriorityQueue<T> implements Collection<T>, Copyable<PriorityQueue<T>> {
    export constructor<T: Comparable<T>>();
    export constructor((T, T) -> int order);

    export override length() -> long;
    export override isEmpty() -> bool;
    export override contains(T value) -> bool;

    // Walks every value once, in no particular order. Taking them out in order means calling `pop`.
    export override makeIterator() -> Iterator<T>;

    export push(T value);

    // The smallest value, taken out. Aborts the program on an empty queue.
    export pop() -> T;

    // The smallest value, left where it is. Aborts the program on an empty queue.
    export peek() -> T;

    export clear();
    export override copy() -> PriorityQueue<T>;
}
```

## SortedMap

```ens
// @std.collections.sortedmap
// Values found by key and walked in the order the keys sort, which is what a Map does not promise.
// Lookup costs more than a Map's in exchange for that order.
export final class SortedMap<K, V> implements Iterable<Entry<K, V>>, Copyable<SortedMap<K, V>> {
    export constructor<K: Comparable<K>>();
    export constructor((K, K) -> int order);

    export length() -> long;
    export isEmpty() -> bool;

    export get(K key) -> V?;
    export set(K key, V value);
    export contains(K key) -> bool;
    export remove(K key) -> bool;
    export removeWhere((K, V) -> bool test) -> long;
    export clear();
    export getOrInsert(K key, () -> V make) -> V;

    // The smallest and largest keys. Both abort the program on an empty map.
    export firstKey() -> K;
    export lastKey() -> K;

    // Views onto the live map, walked in key order without copying anything. Changing the map while
    // one of these is being walked aborts the program.
    export keys() -> Iterable<K>;
    export values() -> Iterable<V>;

    export override makeIterator() -> Iterator<Entry<K, V>>;
    export override copy() -> SortedMap<K, V>;
}
```

## Decisions embodied here

The ladder is two levels and read-only; no MutableCollection split, no indexed Sequence.
`Map` implements `Iterable<Entry>` but not `Collection<Entry>`, because inheriting a contains over entries is useless when callers want it over keys.
`Pair` is renamed `Entry<K, V>` with `const` fields.
Mutating a map or set while walking it aborts the program, detected by a modification counter; today it silently returns wrong results.
`indexOf` answers -1 rather than `long?`, matching `string.indexOf`.
No `firstOrNull`, `lastOrNull`, or `popOrNull`; `first`, `last`, and `pop` abort on empty per the contract rule.
`push`, `pop`, and `pushAll` on `List`; `add` on `Set`, deliberately, since a set has no position.
`removeWhere` on `List`, `Set`, `Map`, and `SortedMap` (ratified 2026-09-02) removes every match in one pass, because a walk that removes from its own container aborts once walks are live views.
Removing the first match or the first few stays index and key based (`indexWhere` plus `removeAt`, `remove(key)`, `firstKey`), so no walk is involved and nothing aborts.
It was weighed against the standing worry that closures get overused: it is a leaf predicate like `indexWhere`, and no further closure-taking method is added without the same weighing.
Collections compare structurally and cannot be map keys; mutable collections join the rejected-key-type list in the compiler.
A struct is refused as a key when any of its fields, transitively, holds an array or a collection (ratified 2026-09-02).
Sorting lives on `List` as a comparator overload plus a conditional natural-order overload; there is no sorting module.
`binarySearch` was cut and can return later.
`PriorityQueue` is a min-heap and its iteration order is unspecified; both facts are documented because a max-heap default and an ordered-looking iterator are recurring surprises elsewhere.
`Deque` has indexed `get`, constant time on a ring buffer, but no `set`, because writing through a position is a list operation.
Hash seeding, growth factors, and whether `Set` shares the map table are implementation notes rather than API, and stay undocumented so they can change.
