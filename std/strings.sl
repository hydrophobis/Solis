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

// A float with exactly `digits` places after the point (0 to 30), rounded:
// `fixed(3.14159, 2)` is "3.14", `fixed(2.0, 1)` is "2.0".
extern func fixed(x: float, digits: int): str;

// Parses a decimal integer, with an optional leading `-`. Returns 0 when the
// string is not one, so check with `isInt` first if that matters.
extern func toInt(s: str): int;
extern func isInt(s: str): bool;

// The same for decimal numbers: `3`, `-2.5`, `1e3`, `.5`. Surrounding spaces
// are ignored; hex, `inf` and `nan` are not numbers here. 0.0 when it is not
// one, so check with `isFloat` first if that matters.
extern func toFloat(s: str): float;
extern func isFloat(s: str): bool;

// The code point of the first character, or -1 for the empty string.
extern func code(s: str): int;

// The one-character string for a code point. Empty for anything that is not
// a Unicode scalar value.
extern func fromCode(n: int): str;

// The UTF-8 bytes of the string, each 0 to 255, and the reverse. Use these
// for protocols and binary formats; everything else in this module counts
// characters.
extern func bytes(s: str): [int];

// How many bytes the string takes in UTF-8, as opposed to `len`, which counts
// characters. Cheap: nothing is allocated.
extern func byteLen(s: str): int;
extern func fromBytes(b: [int]): str;

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

// The text split into lines. \r\n and \n both end a line, and a trailing
// newline does not add an empty one.
func lines(s: str): [str] {
    var text = replace(s, "\r\n", "\n");
    if endsWith(text, "\n") {
        text = sub(text, 0, len(text) - 1);
    }
    if len(text) == 0 {
        var none: [str] = [];
        return none;
    }
    return split(text, "\n");
}

func isSpace(c: str): bool {
    return c == " " || c == "\t" || c == "\n" || c == "\r";
}

// Only the start or only the end of the string trimmed.
func trimStart(s: str): str {
    var i = 0;
    while i < len(s) && isSpace(at(s, i)) {
        i += 1;
    }
    return sub(s, i, len(s));
}

func trimEnd(s: str): str {
    var j = len(s);
    while j > 0 && isSpace(at(s, j - 1)) {
        j -= 1;
    }
    return sub(s, 0, j);
}

// Whether the string is empty or only whitespace.
func isBlank(s: str): bool {
    return len(trim(s)) == 0;
}

// The first letter in upper case and the rest unchanged.
func capitalize(s: str): str {
    if len(s) == 0 {
        return s;
    }
    return upper(sub(s, 0, 1)) + sub(s, 1, len(s));
}

// Removes `prefix` / `suffix` if the string has it, otherwise returns it as is.
func removePrefix(s: str, prefix: str): str {
    if startsWith(s, prefix) {
        return sub(s, len(prefix), len(s));
    }
    return s;
}

func removeSuffix(s: str, suffix: str): str {
    if endsWith(s, suffix) {
        return sub(s, 0, len(s) - len(suffix));
    }
    return s;
}

// Case-insensitive equality (ASCII and the other scripts `lower` handles).
func equalsIgnoreCase(a: str, b: str): bool {
    return lower(a) == lower(b);
}
