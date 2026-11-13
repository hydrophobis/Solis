func main() {
    let limit = 10000000;
    var flags = [true];
    var i = 1;
    while i < limit { push(flags, true); i += 1; }
    var count = 0;
    var n = 2;
    while n < limit {
        if flags[n] {
            count += 1;
            var m = n + n;
            while m < limit { flags[m] = false; m += n; }
        }
        n += 1;
    }
    print("${count}");
}
