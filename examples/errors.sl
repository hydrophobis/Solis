// Deliberately broken, to exercise recovery.
struct Point {
    x: int;
    y int;
}

func dist(a: Point, b: Point): float {
    let dx = a.x - b.x
    return sqrt(dx * dx);
}

func oops() {
    compute() = 4;
}

func fine(n: int): int {
    return n * 2;
}
