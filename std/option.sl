// No isSome/unwrap/etc: enums can't have methods, and a generic function
// can't yet take an already-generic type like `Option[T]` as a parameter.
// Use `switch` to unwrap.

enum Option[T] {
    Some(value: T),
    None,
}
