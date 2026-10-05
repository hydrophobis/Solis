struct Vec2 {
    x: float;
    y: float;

    func length(): float {
        return sqrt(this.x * this.x + this.y * this.y);
    }

    mut func scale(k: float) {
        this.x *= k;
        this.y *= k;
    }

    static func zero(): Vec2 {
        return Vec2 { x: 0.0, y: 0.0 };
    }
}

enum Shape {
    Circle(r: float),
    Rect(w: float, h: float),
    Point,
}

// Exhaustive: omit a variant and the compiler names it.
func area(s: Shape): float {
    switch s {
        case Shape.Circle(r):
            return 3.14159 * r * r;

        case Shape.Rect(w, h):
            return w * h;

        case Shape.Point:
            return 0.0;
    }
}

func describe(s: Shape): str {
    switch s {
        case Shape.Circle(r):
            return "circle of radius ${r}";
        case Shape.Rect(w, h):
            return "${w} by ${h} rectangle";
        case Shape.Point:
            return "a point";
    }
}

func sum(xs: [int]): int {
    var total = 0;
    for x in xs {
        total += x;
    }
    return total;
}

func fib(n: int): int {
    if n < 2 {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

func main() {
    print("--- Solis ---");

    var v = Vec2 { x: 3.0, y: 4.0 };
    print("length = ${v.length()}");
    v.scale(2.0);
    print("after scale, length = ${v.length()}");

    let shapes = [Shape.Circle(2.0), Shape.Rect(3.0, 4.0), Shape.Point];
    for s in shapes {
        print("${describe(s)} has area ${area(s)}");
    }

    let nums = [1, 2, 3, 4, 5];
    print("sum = ${sum(nums)}");

    var i = 0;
    while i < 8 {
        print("fib(${i}) = ${fib(i)}");
        i += 1;
    }
}
