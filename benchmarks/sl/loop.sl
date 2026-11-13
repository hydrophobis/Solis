func main() {
    var i = 0;
    var s = 0;
    while i < 100000000 {
        s += i % 7;
        i += 1;
    }
    print("${s}");
}
