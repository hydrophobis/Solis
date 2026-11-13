func main() {
    var s = "";
    var i = 0;
    var n = 0;
    while i < 300000 {
        let t = "item ${i}: " + "x";
        n += 1;
        if i % 1000 == 0 { s = t; }
        i += 1;
    }
    print("${n} ${s}");
}
