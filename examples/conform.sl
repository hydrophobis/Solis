// Exercises the corners the other examples skip: break and continue,
// compound assignment to an element, multi-pattern cases, module-level
// bindings, nested data, and the string primitives. Both runtimes must agree
// on every line, which is what `tools/conform.sh` checks.

import strings;

let limit = 5;
var counter = 0;

struct Point {
    x: int;
    y: int;

    func manhattan(): int {
        return absI(this.x) + absI(this.y);
    }

    static func origin(): Point {
        return Point { x: 0, y: 0 };
    }
}

struct Line {
    a: Point;
    b: Point;
}

enum Token {
    Num(v: int),
    Word(s: str),
    Open,
    Close,
    End,
}

func absI(n: int): int {
    if n < 0 {
        return 0 - n;
    }
    return n;
}

// break and continue in a while loop.
func firstMultiple(of: int, above: int): int {
    var n = above;
    while true {
        n += 1;
        if n % of != 0 {
            continue;
        }
        break;
    }
    return n;
}

// break and continue in a for loop, which is desugared to an index loop.
func sumUntilNegative(xs: [int]): int {
    var total = 0;
    for x in xs {
        if x == 0 {
            continue;
        }
        if x < 0 {
            break;
        }
        total += x;
    }
    return total;
}

// Deep but legal recursion. Both runtimes cap the call depth at 256 and
// report it with the same message; this stays comfortably under that, so it
// checks that frames are built and unwound the same way on both.
func depth(n: int): int {
    if n <= 0 {
        return 0;
    }
    return 1 + depth(n - 1);
}

// A case listing several variants.
func isDelimiter(t: Token): bool {
    switch t {
        case Token.Open, Token.Close, Token.End:
            return true;
        case Token.Num(v):
            return v == 0;
        case Token.Word(s):
            return strings.len(s) == 0;
    }
}

func render(t: Token): str {
    switch t {
        case Token.Num(v):
            return "num:${v}";
        case Token.Word(s):
            return "word:${s}";
        case Token.Open:
            return "(";
        case Token.Close:
            return ")";
        case Token.End:
            return ".";
    }
}

func main() {
    print("--- loops ---");
    print("first multiple of 7 above 20 = ${firstMultiple(7, 20)}");
    print("sum until negative = ${sumUntilNegative([3, 0, 4, -1, 100])}");

    // Compound assignment to an element.
    var tallies = [0, 0, 0];
    var i = 0;
    while i < 9 {
        tallies[i % 3] += i;
        i += 1;
    }
    print("tallies = ${tallies}");

    // Nested structs are values, so a copy does not alias.
    var p = Point { x: 3, y: -4 };
    let l = Line { a: p, b: Point.origin() };
    p.x = 99;
    print("manhattan = ${l.a.manhattan()}, p.x = ${p.x}, l.a.x = ${l.a.x}");

    // Arrays are references, so a copy does alias.
    let shared = [1, 2, 3];
    let alias = shared;
    alias[0] = 42;
    print("shared[0] = ${shared[0]}, len = ${len(shared)}");

    print("depth(200) = ${depth(200)}");

    print("--- enums ---");
    let toks = [
        Token.Open,
        Token.Num(17),
        Token.Word("solis"),
        Token.Num(0),
        Token.Word(""),
        Token.Close,
        Token.End,
    ];
    for t in toks {
        print("${render(t)} delim=${isDelimiter(t)}");
    }

    print("--- globals ---");
    var n = 0;
    while n < limit {
        counter += n;
        n += 1;
    }
    print("limit = ${limit}, counter = ${counter}");

    print("--- strings ---");
    let s = "  Hello, Solis  ";
    print("trim     = '${strings.trim(s)}'");
    print("upper    = ${strings.upper(strings.trim(s))}");
    print("len      = ${strings.len(strings.trim(s))}");
    print("sub      = ${strings.sub(strings.trim(s), 7, 12)}");
    print("indexOf = ${strings.indexOf(s, "Solis")}");
    print("replace  = ${strings.replace("a-b-c", "-", "+")}");
    print("reverse  = ${strings.reverse("stressed")}");
    print("split    = ${strings.split("1,2,3", ",")}");
    print("join     = ${strings.join(strings.split("a b c", " "), "/")}");
    print("chars    = ${strings.chars("abc")}");
    print("fromInt = ${strings.fromInt(-42)}");
    print("toInt   = ${strings.toInt(" 123 ")}, isInt = ${strings.isInt("12x")}");

    print("--- mixed ---");
    print("nested = ${[[1, 2], [3], [4, 5, 6]]}");
    print("bools  = ${true} ${false} ${!true}");
    print("floats = ${1.0 / 3.0} ${0.0 - 2.5} ${1e10}");
    print("ints   = ${7 / 2} ${7 % 2} ${0 - 7 / 2}");
    print("cmp    = ${"a" == "a"} ${"a" != "b"} ${3 < 4} ${4.5 >= 4.5}");
    print("logic  = ${true && false} ${true || false}");
}
