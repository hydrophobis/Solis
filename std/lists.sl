// The `lists` module: helpers for arrays.
//
// Everything here returns a new array and leaves its arguments alone. The
// language has no function values, so there is no `map` or `filter`; write
// the loop. What is here is what a loop would otherwise have to repeat.

import math;

func concat[T](a: [T], b: [T]): [T] {
    var out: [T] = [];
    for x in a {
        push(out, x);
    }
    for x in b {
        push(out, x);
    }
    return out;
}

func flatten[T](parts: [[T]]): [T] {
    var out: [T] = [];
    for part in parts {
        for x in part {
            push(out, x);
        }
    }
    return out;
}

func reversed[T](xs: [T]): [T] {
    var out: [T] = [];
    var i = len(xs) - 1;
    while i >= 0 {
        push(out, xs[i]);
        i -= 1;
    }
    return out;
}

// `n` copies of `x`.
func repeat[T](x: T, n: int): [T] {
    var out: [T] = [];
    var i = 0;
    while i < n {
        push(out, x);
        i += 1;
    }
    return out;
}

// Consecutive pieces of at most `size` elements.
func chunks[T](xs: [T], size: int): [[T]] {
    var out: [[T]] = [];
    if size <= 0 {
        return out;
    }
    var i = 0;
    while i < len(xs) {
        var end = i + size;
        if end > len(xs) {
            end = len(xs);
        }
        push(out, xs[i:end]);
        i = end;
    }
    return out;
}

// The first and last element, or `fallback` for an empty array.
func firstOr[T](xs: [T], fallback: T): T {
    if len(xs) == 0 {
        return fallback;
    }
    return xs[0];
}

func lastOr[T](xs: [T], fallback: T): T {
    if len(xs) == 0 {
        return fallback;
    }
    return xs[len(xs) - 1];
}

func sumInt(xs: [int]): int {
    var total = 0;
    for x in xs {
        total += x;
    }
    return total;
}

func sumFloat(xs: [float]): float {
    var total = 0.0;
    for x in xs {
        total += x;
    }
    return total;
}

// The smallest and largest element, or `fallback` for an empty array.
func minInt(xs: [int], fallback: int): int {
    if len(xs) == 0 {
        return fallback;
    }
    var best = xs[0];
    for x in xs {
        if x < best {
            best = x;
        }
    }
    return best;
}

func maxInt(xs: [int], fallback: int): int {
    if len(xs) == 0 {
        return fallback;
    }
    var best = xs[0];
    for x in xs {
        if x > best {
            best = x;
        }
    }
    return best;
}

func minFloat(xs: [float], fallback: float): float {
    if len(xs) == 0 {
        return fallback;
    }
    var best = xs[0];
    for x in xs {
        if x < best {
            best = x;
        }
    }
    return best;
}

func maxFloat(xs: [float], fallback: float): float {
    if len(xs) == 0 {
        return fallback;
    }
    var best = xs[0];
    for x in xs {
        if x > best {
            best = x;
        }
    }
    return best;
}

// The mean, or 0.0 for an empty array.
func mean(xs: [float]): float {
    if len(xs) == 0 {
        return 0.0;
    }
    return sumFloat(xs) / math.toFloat(len(xs));
}

// Without repeats, keeping the first occurrence of each.
func uniqueInts(xs: [int]): [int] {
    var seen: {int: bool} = {};
    var out: [int] = [];
    for x in xs {
        if !contains(seen, x) {
            seen[x] = true;
            push(out, x);
        }
    }
    return out;
}

func uniqueStrs(xs: [str]): [str] {
    var seen: {str: bool} = {};
    var out: [str] = [];
    for x in xs {
        if !contains(seen, x) {
            seen[x] = true;
            push(out, x);
        }
    }
    return out;
}

// A sorted copy; the argument is untouched.
func sortedInts(xs: [int]): [int] {
    var out = xs[:];
    sort(out);
    return out;
}

func sortedFloats(xs: [float]): [float] {
    var out = xs[:];
    sort(out);
    return out;
}

func sortedStrs(xs: [str]): [str] {
    var out = xs[:];
    sort(out);
    return out;
}
