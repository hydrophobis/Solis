func main() {
    var a = [0];
    var i = 1;
    while i < 2000000 { push(a, i); i += 1; }
    var s = 0;
    for x in a { s += x; }
    var r = 0;
    while r < 5 {
        i = 0;
        while i < len(a) { a[i] = a[i] + 1; i += 1; }
        r += 1;
    }
    print("${s} ${a[5]}");
}
