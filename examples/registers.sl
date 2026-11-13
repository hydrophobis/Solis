// Register-VM regression: structs, enums, maps, strings, slices, branches, early exits.
struct V { x: int; y: int; mut func bump(k: int) { this.x += k; this.y -= k; } func sum(): int { return this.x + this.y; } }
struct Box { v: V; name: str; tags: [str]; }
enum Shape { Dot, Seg(a: V, b: V), Named(label: str, n: int) }
func describe(s: Shape): str {
    switch s {
        case Shape.Dot: return "dot";
        case Shape.Seg(a, b): return "seg ${a.x},${a.y}->${b.x},${b.y}";
        case Shape.Named(label, n): return "${label}#${n}";
    }
}
func mk(i: int): Box { return Box { v: V { x: i, y: i * 2 }, name: "b${i}", tags: ["t", "u${i}"] }; }
func fact(n: int): int { if n <= 1 { return 1; } return n * fact(n - 1); }
func pick(xs: [int], i: int): int { if i < 0 || i >= len(xs) { return -1; } return xs[i]; }
func main() {
    var b = mk(3);
    var c = b;
    c.v.bump(5);
    b.name = b.name + "!" + "?";
    push(c.tags, "extra");
    print("${b.v.x} ${b.v.y} ${c.v.x} ${c.v.y} ${b.name} ${len(b.tags)} ${len(c.tags)}");
    var arr = [mk(1), mk(2), mk(3)];
    var i = 0;
    var tot = 0;
    while i < len(arr) { arr[i].v.bump(i); tot += arr[i].v.sum(); i += 1; }
    print("tot ${tot} ${arr[2].v.x}");
    var shapes = [Shape.Dot, Shape.Seg(V { x: 1, y: 2 }, V { x: 3, y: 4 }), Shape.Named("n", 7)];
    for s in shapes { print(describe(s)); }
    var m: {str: int} = {"a": 1};
    m["b"] = 2; m["a"] += 10;
    print("${m["a"]} ${m["b"]} ${len(m)}");
    var s = "";
    var k = 0;
    while k < 5 { if k == 3 { k += 1; continue; } s = s + "${k},"; k += 1; if k > 10 { break; } }
    print(s);
    let sl = [10, 20, 30, 40, 50][1:4];
    print("${sl} ${len(sl)} ${pick(sl, 2)} ${pick(sl, 9)} ${fact(10)}");
    var flag = true && (3 > 2 || 1 / 1 == 0);
    var f2 = !flag || (4 < 3 && flag);
    print("${flag} ${f2} ${1.5 * 2.0 > 2.9 && 2.0 < 3.0}");
    var fl = 0.0; var q = 0;
    while q < 10 { fl += 0.25 * 3.0; q += 1; }
    print("${fl}");
}
