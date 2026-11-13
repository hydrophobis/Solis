// Conditions compiled as branches: &&, ||, ! nested in if and while, ints and floats.
func t(a: int, b: int, c: float): int {
    var r = 0;
    if a > 1 || b > 1 { r += 1; }
    if a > 1 && b > 1 { r += 10; }
    if !(a > 1 || b > 1) { r += 100; }
    if a > 1 && (b > 1 || c > 1.0) { r += 1000; }
    if (a > 1 || b > 1) && c < 5.0 { r += 10000; }
    if c > 1.0 || c < 0.0 { r += 100000; }
    if a == b || !(c > 0.5) { r += 1000000; } else { r += 7000000; }
    return r;
}
func main() {
    var a = 0; var tot = 0;
    while a < 4 {
        var b = 0;
        while b < 4 && !(a == 3 && b == 2) {
            tot += t(a, b, cval(a, b));
            b += 1;
        }
        a += 1;
    }
    print("${tot}");
}
func cval(a: int, b: int): float {
    var f = 0.0; var k = 0;
    while k < a + b { f += 0.5; k += 1; }
    return f;
}
