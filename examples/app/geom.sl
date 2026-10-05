// A module. Everything declared here is reachable as `geom.<name>`.

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

    static func of(x: float, y: float): Vec2 {
        return Vec2 { x: x, y: y };
    }
}

enum Shape {
    Circle(r: float),
    Rect(w: float, h: float),
}

func area(s: Shape): float {
    switch s {
        case Shape.Circle(r):
            return 3.14159 * r * r;
        case Shape.Rect(w, h):
            return w * h;
    }
}
