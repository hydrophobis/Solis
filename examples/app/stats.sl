// A second module, independent of the first.

func sum(xs: [int]): int {
    var t = 0;
    for x in xs {
        t += x;
    }
    return t;
}

func max(xs: [int]): int {
    var best = xs[0];
    for x in xs {
        if x > best {
            best = x;
        }
    }
    return best;
}

// Integer mean: Solis will not convert int to float implicitly, which is
// exactly the error this used to have.
func mean(xs: [int]): int {
    return sum(xs) / len(xs);
}
