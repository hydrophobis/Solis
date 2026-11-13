struct Body { x: float; y: float; vx: float; vy: float; }
func step(bodies: [Body], dt: float): float {
    var i = 0;
    var total = 0.0;
    while i < len(bodies) {
        var b = bodies[i];
        b.x += b.vx * dt;
        b.y += b.vy * dt;
        if b.x > 100.0 || b.x < 0.0 { b.vx = 0.0 - b.vx; }
        if b.y > 100.0 || b.y < 0.0 { b.vy = 0.0 - b.vy; }
        bodies[i] = b;
        total += b.x + b.y;
        i += 1;
    }
    return total;
}
func main() {
    var bodies = [Body { x: 1.0, y: 1.0, vx: 1.5, vy: 0.5 }];
    var k = 1;
    while k < 1000 { push(bodies, Body { x: 1.0, y: 2.0, vx: 1.5, vy: 0.5 }); k += 1; }
    var frame = 0;
    var acc = 0.0;
    while frame < 5000 { acc += step(bodies, 0.016); frame += 1; }
    print("${acc}");
}
