func main() {
    var m: {int: int} = {0: 0};
    var i = 1;
    while i < 3000 { m[i] = i * 2; i += 1; }
    var s = 0;
    var r = 0;
    while r < 2000 {
        i = 0;
        while i < 3000 { s += m[i]; i += 1; }
        r += 1;
    }
    print("${s}");
}
