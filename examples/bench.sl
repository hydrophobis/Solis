// A benchmark comparing interpreting the source against running the
// compiled image.
//
//   solis examples/bench.sl
//   solis -o bench.slb examples/bench.sl && solis bench.slb

struct Body {
    x: float;
    y: float;
    vx: float;
    vy: float;
}

func fib(n: int): int {
    if n < 2 {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

// Integer work: loops, arithmetic, element access.
func sieve(limit: int): int {
    var flags = [true];
    var i = 1;
    while i < limit {
        push(flags, true);
        i += 1;
    }
    var count = 0;
    var n = 2;
    while n < limit {
        if flags[n] {
            count += 1;
            var m = n + n;
            while m < limit {
                flags[m] = false;
                m += n;
            }
        }
        n += 1;
    }
    return count;
}

// Float work through struct fields, which is the shape most embedded scripts
// actually have: a handful of values updated every frame.
func step(bodies: [Body], dt: float): float {
    var i = 0;
    var total = 0.0;
    while i < len(bodies) {
        var b = bodies[i];
        b.x += b.vx * dt;
        b.y += b.vy * dt;
        if b.x > 100.0 || b.x < 0.0 {
            b.vx = 0.0 - b.vx;
        }
        if b.y > 100.0 || b.y < 0.0 {
            b.vy = 0.0 - b.vy;
        }
        bodies[i] = b;
        total += b.x + b.y;
        i += 1;
    }
    return total;
}

func main() {
    print("fib(24) = ${fib(24)}");
    print("primes below 20000 = ${sieve(20000)}");

    var bodies = [Body { x: 1.0, y: 1.0, vx: 1.5, vy: 0.5 }];
    var k = 1;
    while k < 200 {
        let f = 1.0 + 1.0;
        push(bodies, Body { x: 1.0, y: 2.0, vx: 1.5, vy: 0.5 });
        k += 1;
    }

    var frame = 0;
    var acc = 0.0;
    while frame < 300 {
        acc += step(bodies, 0.016);
        frame += 1;
    }
    print("bodies = ${len(bodies)}, checksum = ${acc > 0.0}");
}
