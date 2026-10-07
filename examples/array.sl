// Array builtins: pop/indexOf/contains/reverse/removeAt/insertAt/sort.

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
    var a = [5, 3, 1, 4, 2];

    print("contains 4 = ${contains(a, 4)}");
    print("indexOf 1 = ${indexOf(a, 1)}");

    sort(a);
    print("sorted = ${show(a)}");

    reverse(a);
    print("reversed = ${show(a)}");

    let popped = pop(a);
    print("popped = ${popped}, now = ${show(a)}");

    removeAt(a, 0);
    print("after removeAt(0) = ${show(a)}");

    insertAt(a, 1, 99);
    print("after insertAt(1, 99) = ${show(a)}");
}
