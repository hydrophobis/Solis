// The `rand` module: pseudo-random numbers. xorshift64*, seeded from the
// clock, reproducible once `seed` is called. Not cryptographic. Only the
// standard interpreter opens this.

import strings;

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

// A random element, or `fallback` for an empty array.
func pick[T](xs: [T], fallback: T): T {
    if len(xs) == 0 {
        return fallback;
    }
    return xs[below(len(xs))];
}

// Shuffles the array in place (Fisher-Yates).
func shuffle[T](xs: [T]) {
    var i = len(xs) - 1;
    while i > 0 {
        let j = below(i + 1);
        let tmp = xs[i];
        xs[i] = xs[j];
        xs[j] = tmp;
        i -= 1;
    }
}

// A random version 4 UUID such as "3f2b8c1e-9d4a-4b6f-8a1c-5e7d2f9a0b34".
// Not cryptographically secure, like everything in this module.
func uuid(): str {
    let digits = "0123456789abcdef";
    var out = "";
    var i = 0;
    while i < 36 {
        if i == 8 || i == 13 || i == 18 || i == 23 {
            out = out + "-";
        } else if i == 14 {
            out = out + "4";
        } else if i == 19 {
            let v = 8 + below(4);
            out = out + strings.sub(digits, v, v + 1);
        } else {
            let v = below(16);
            out = out + strings.sub(digits, v, v + 1);
        }
        i += 1;
    }
    return out;
}
