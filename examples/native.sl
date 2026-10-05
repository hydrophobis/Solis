// The scalar core: what the first C runtime covers.
// int, float, bool, locals, control flow, calls, recursion.

func fib(n: int): int {
    if n < 2 {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

func gcd(a: int, b: int): int {
    var x = a;
    var y = b;
    while y != 0 {
        let t = y;
        y = x % y;
        x = t;
    }
    return x;
}

func isPrime(n: int): bool {
    if n < 2 {
        return false;
    }
    var d = 2;
    while d * d <= n {
        if n % d == 0 {
            return false;
        }
        d += 1;
    }
    return true;
}

func area(r: float): float {
    return 3.14159265 * r * r;
}

func main() {
    print(fib(20));
    print(gcd(1071, 462));
    print(isPrime(97), isPrime(91));
    print(area(2.0));

    var i = 0;
    var total = 0;
    while i < 1000 {
        if isPrime(i) {
            total += i;
        }
        i += 1;
    }
    print(total);
}
