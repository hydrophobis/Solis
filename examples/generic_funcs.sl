// Generic functions, unified from the call's own arguments (no turbofish).

func identity[T](x: T): T {
    return x;
}

func pair[A, B](a: A, b: B): str {
    return "(${a}, ${b})";
}

func firstOf[T](items: [T]): T {
    return items[0];
}

func main() {
    print("${identity(42)}");
    print(pair(1, "x"));
    print(pair(true, 3.5));
    print("${firstOf([10, 20, 30])}");
    print("${firstOf(["a", "b"])}");
}
