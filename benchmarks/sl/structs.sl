struct P { x: int; y: int; }
func mk(i: int): P { return P { x: i, y: i + 1 }; }
func main() {
    var i = 0;
    var s = 0;
    while i < 5000000 {
        let p = mk(i);
        s += p.x + p.y;
        i += 1;
    }
    print("${s}");
}
