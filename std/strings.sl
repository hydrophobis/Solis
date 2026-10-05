// The `strings` module.
//
// Named `strings` rather than `str` so it cannot be confused with the `str`
// type. Indices are in characters, not bytes, so non-ASCII text behaves the
// way a reader expects.
//
// There are no optionals, so searches return -1 for "not found" rather than
// null, and `toInt` pairs with `isInt` instead of failing.

extern func len(s: str): int;

// Characters from `start` up to but not including `end`. Out-of-range
// positions are clamped rather than being an error.
extern func sub(s: str, start: int, end: int): str;

// The character at `i`, as a one-character string. Empty if out of range.
extern func at(s: str, i: int): str;

// Character index of the first occurrence, or -1.
extern func indexOf(s: str, needle: str): int;
extern func contains(s: str, needle: str): bool;
extern func startsWith(s: str, prefix: str): bool;
extern func endsWith(s: str, suffix: str): bool;

extern func upper(s: str): str;
extern func lower(s: str): str;
extern func trim(s: str): str;
extern func repeat(s: str, n: int): str;
extern func replace(s: str, from: str, to: str): str;
extern func reverse(s: str): str;

extern func split(s: str, sep: str): [str];
extern func join(parts: [str], sep: str): str;
extern func chars(s: str): [str];

extern func fromInt(n: int): str;
extern func fromFloat(x: float): str;
extern func fromBool(b: bool): str;

// Parses a decimal integer, with an optional leading `-`. Returns 0 when the
// string is not one, so check with `isInt` first if that matters.
extern func toInt(s: str): int;
extern func isInt(s: str): bool;

func isEmpty(s: str): bool {
    return len(s) == 0;
}

func padLeft(s: str, width: int, fill: str): str {
    var out = s;
    while len(out) < width {
        out = fill + out;
    }
    return out;
}

func padRight(s: str, width: int, fill: str): str {
    var out = s;
    while len(out) < width {
        out = out + fill;
    }
    return out;
}

func count(s: str, needle: str): int {
    if isEmpty(needle) {
        return 0;
    }
    var n = 0;
    var rest = s;
    var atIndex = indexOf(rest, needle);
    while atIndex >= 0 {
        n += 1;
        rest = sub(rest, atIndex + len(needle), len(rest));
        atIndex = indexOf(rest, needle);
    }
    return n;
}

// Everything before the first `sep`, or the whole string if absent.
func before(s: str, sep: str): str {
    let i = indexOf(s, sep);
    if i < 0 {
        return s;
    }
    return sub(s, 0, i);
}

// Everything after the first `sep`, or the empty string if absent.
func after(s: str, sep: str): str {
    let i = indexOf(s, sep);
    if i < 0 {
        return "";
    }
    return sub(s, i + len(sep), len(s));
}
