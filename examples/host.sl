// Everything below the line is provided by the embedding application.
// A script can reach exactly what the host registers, and nothing else -
// there is no ambient filesystem, network or clock to fall back on.

extern func nowMs(): int;
extern func hostName(): str;
extern func repeat(s: str, n: int): str;

struct Timer {
    start: int;

    static func begin(): Timer {
        return Timer { start: nowMs() };
    }

    func elapsed(): int {
        return nowMs() - this.start;
    }
}

func busy(n: int): int {
    var total = 0;
    var i = 0;
    while i < n {
        total += i % 7;
        i += 1;
    }
    return total;
}

func main() {
    print("running on: ${hostName()}");
    print(repeat("-", 28));

    let t = Timer.begin();
    let r = busy(200000);
    print("busy() = ${r}");
    print("took ${t.elapsed()} ms");

    print(repeat("=", 28));
    print("the host exposed 3 functions; that is the entire sandbox");
}
