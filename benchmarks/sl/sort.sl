func qsort(a: [int], lo: int, hi: int) {
    if lo >= hi { return; }
    let pivot = a[(lo + hi) / 2];
    var i = lo;
    var j = hi;
    while i <= j {
        while a[i] < pivot { i += 1; }
        while a[j] > pivot { j -= 1; }
        if i <= j {
            let t = a[i];
            a[i] = a[j];
            a[j] = t;
            i += 1;
            j -= 1;
        }
    }
    qsort(a, lo, j);
    qsort(a, i, hi);
}
func main() {
    var a = [0];
    var seed = 12345;
    var k = 1;
    while k < 1000000 {
        seed = (seed * 1103515245 + 12345) % 2147483648;
        push(a, seed);
        k += 1;
    }
    qsort(a, 0, len(a) - 1);
    var ok = 0;
    k = 1;
    while k < len(a) { if a[k - 1] <= a[k] { ok += 1; } k += 1; }
    print("${ok} ${a[0]} ${a[len(a) - 1]}");
}
