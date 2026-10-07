func show(a: [int]): str {
    var s = "[";
    var i = 0;
    while i < len(a) {
        if i > 0 { s = s + ", "; }
        s = s + "${a[i]}";
        i += 1;
    }
    return s + "]";
}

func main() {
    let a = [10, 20, 30, 40, 50];

    print("a[-1] = ${a[-1]}");
    print("a[-2] = ${a[-2]}");

    print("a[1:3] = ${show(a[1:3])}");
    print("a[:2] = ${show(a[:2])}");
    print("a[3:] = ${show(a[3:])}");
    print("a[:] = ${show(a[:])}");
    print("a[-2:] = ${show(a[-2:])}");
    print("a[:-1] = ${show(a[:-1])}");
    print("a[1:1] = ${show(a[1:1])}");

    var b = [1, 2, 3];
    b[-1] = 99;
    print("b after a[-1] = 99: ${show(b)}");
}
