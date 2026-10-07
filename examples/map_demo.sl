// Map type: construction, indexing, has/remove/keys/values, and std/map.sl's
// merge built on top of the primitives.

import map;

func main() {
    var m: {str: int} = {"a": 1, "b": 2};
    print("m[a] = ${m["a"]}");

    m["c"] = 3;
    print("len after set = ${len(m)}");
    print("has c = ${contains(m, "c")}");
    print("has z = ${contains(m, "z")}");

    m["a"] += 10;
    print("m[a] after += 10 = ${m["a"]}");

    let removed = removeKey(m, "b");
    print("removed b = ${removed}, len now = ${len(m)}");

    let other: {str: int} = {"c": 300, "d": 4};
    let merged = map.merge(m, other);
    print("merged = ${merged}");
}
