// Maps past the linear-scan size: indexed lookup, overwrite, removal, string and float keys.
import map;
func main() {
    var m: {int: int} = {0: 0};
    var i = 1;
    while i < 1000 { m[i] = i * 3; i += 1; }
    var bad = 0;
    i = 0;
    while i < 1000 { if m[i] != i * 3 { bad += 1; } i += 1; }
    print("len ${len(m)} bad ${bad}");
    i = 0;
    while i < 1000 { if i % 3 == 0 { removeKey(m, i); } i += 1; }
    print("after remove len ${len(m)} has3 ${contains(m, 3)} has4 ${contains(m, 4)} has999 ${contains(m, 999)}");
    i = 0;
    var s = 0;
    while i < 1000 { if contains(m, i) { s += m[i]; } i += 1; }
    print("sum ${s}");
    m[3] = 77;
    m[4] = 88;
    print("${m[3]} ${m[4]} ${len(m)}");
    var sm: {str: int} = {"a": 1};
    i = 0;
    while i < 200 { sm["k${i}"] = i; i += 1; }
    sm["k150"] += 1000;
    print("${len(sm)} ${sm["k150"]} ${sm["a"]} ${contains(sm, "k199")} ${contains(sm, "k200")}");
    var fm: {float: str} = {0.5: "half"};
    i = 1;
    while i < 30 { fm[0.5 + 1.0 * map_f(i)] = "x${i}"; i += 1; }
    print("${len(fm)} ${fm[0.5]} ${fm[3.5]}");
}
func map_f(i: int): float { return 0.0 + 1.0 * conv(i); }
func conv(i: int): float { var f = 0.0; var k = 0; while k < i { f += 1.0; k += 1; } return f; }
