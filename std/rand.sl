// The `rand` module: pseudo-random numbers. xorshift64*, seeded from the
// clock, reproducible once `seed` is called. Not cryptographic. Only the
// standard interpreter opens this.

// Fixes the sequence. Zero is replaced, because it is a fixed point of the
// generator rather than a seed.
extern func seed(n: int);

// A number in 0 ..< n, uniformly. Zero when `n` is not positive.
extern func below(n: int): int;

// A float in 0.0 ..< 1.0.
extern func real(): float;

// A number in lo ..< hi, or `lo` if that range is empty.
func between(lo: int, hi: int): int {
    if hi <= lo {
        return lo;
    }
    return lo + below(hi - lo);
}

// True with probability `p`.
func chance(p: float): bool {
    return real() < p;
}

// A float in lo ..< hi.
func realBetween(lo: float, hi: float): float {
    return lo + real() * (hi - lo);
}

// A valid index into an array of `count` items, or -1 when it is empty.
func index(count: int): int {
    if count <= 0 {
        return 0 - 1;
    }
    return below(count);
}
