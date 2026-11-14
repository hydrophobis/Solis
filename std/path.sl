// The `path` module: building and taking apart file paths as text.
//
// Nothing here touches the disk; pair it with `fs`. Both `/` and `\` are
// read as separators, so a Windows path works anywhere, and everything this
// module produces uses `/`, which every platform's file functions accept.

import strings;

func isSep(c: str): bool {
    return c == "/" || c == "\\";
}

// `a` and `b` with exactly one separator between them. An absolute `b`
// replaces `a`, and an empty side is ignored.
func join(a: str, b: str): str {
    if strings.isEmpty(a) {
        return b;
    }
    if strings.isEmpty(b) {
        return a;
    }
    if isAbs(b) {
        return b;
    }
    var left = a;
    while strings.len(left) > 1 && isSep(strings.at(left, strings.len(left) - 1)) {
        left = strings.sub(left, 0, strings.len(left) - 1);
    }
    if isSep(strings.at(left, strings.len(left) - 1)) {
        return left + b;
    }
    return left + "/" + b;
}

// Joins any number of parts left to right.
func joinAll(parts: [str]): str {
    var out = "";
    for p in parts {
        out = join(out, p);
    }
    return out;
}

// Starts at the root: `/usr`, `\share`, or `C:/dir`.
func isAbs(p: str): bool {
    if strings.isEmpty(p) {
        return false;
    }
    if isSep(strings.at(p, 0)) {
        return true;
    }
    return strings.len(p) >= 3 && strings.at(p, 1) == ":" && isSep(strings.at(p, 2));
}

// Index just past the last separator, or 0 when there is none.
func lastSep(p: str): int {
    var i = strings.len(p) - 1;
    while i >= 0 {
        if isSep(strings.at(p, i)) {
            return i;
        }
        i -= 1;
    }
    return -1;
}

// `p` without trailing separators (but a bare root stays as it is).
func trimEnd(p: str): str {
    var out = p;
    while strings.len(out) > 1 && isSep(strings.at(out, strings.len(out) - 1)) {
        out = strings.sub(out, 0, strings.len(out) - 1);
    }
    return out;
}

// The last component: `a/b/c.txt` -> `c.txt`.
func base(p: str): str {
    let t = trimEnd(p);
    let i = lastSep(t);
    if i < 0 {
        return t;
    }
    if i == 0 && strings.len(t) == 1 {
        return t;
    }
    return strings.sub(t, i + 1, strings.len(t));
}

// Everything before the last component: `a/b/c.txt` -> `a/b`. `.` when there
// is no directory part.
func dir(p: str): str {
    let t = trimEnd(p);
    let i = lastSep(t);
    if i < 0 {
        return ".";
    }
    if i == 0 {
        return "/";
    }
    return trimEnd(strings.sub(t, 0, i));
}

// The extension with its dot, or "" when there is none. A name that only
// starts with a dot, like `.gitignore`, has no extension.
func ext(p: str): str {
    let name = base(p);
    var i = strings.len(name) - 1;
    while i > 0 {
        if strings.at(name, i) == "." {
            return strings.sub(name, i, strings.len(name));
        }
        i -= 1;
    }
    return "";
}

// The last component without its extension: `a/c.tar.gz` -> `c.tar`.
func stem(p: str): str {
    let name = base(p);
    let e = ext(p);
    return strings.sub(name, 0, strings.len(name) - strings.len(e));
}

func withExt(p: str, newExt: str): str {
    let d = dir(p);
    let name = stem(p) + newExt;
    if strings.contains(p, "/") || strings.contains(p, "\\") {
        return join(d, name);
    }
    return name;
}

// The components, without empty ones: `/a//b/` -> `a`, `b`.
func parts(p: str): [str] {
    var out: [str] = [];
    var cur = "";
    for c in strings.chars(p) {
        if isSep(c) {
            if !strings.isEmpty(cur) {
                push(out, cur);
            }
            cur = "";
        } else {
            cur = cur + c;
        }
    }
    if !strings.isEmpty(cur) {
        push(out, cur);
    }
    return out;
}

// Resolves `.` and `..` and collapses repeated separators, without looking
// at the disk. A `..` that has nothing to remove stays at the front of a
// relative path and disappears at the root.
func clean(p: str): str {
    if strings.isEmpty(p) {
        return ".";
    }
    let absolute = isAbs(p);
    var drive = "";
    var rest = p;
    if strings.len(p) >= 2 && strings.at(p, 1) == ":" {
        drive = strings.sub(p, 0, 2);
        rest = strings.sub(p, 2, strings.len(p));
    }
    var out: [str] = [];
    for part in parts(rest) {
        if part == "." {
            continue;
        }
        if part == ".." {
            if len(out) > 0 && out[len(out) - 1] != ".." {
                pop(out);
            } else if !absolute {
                push(out, "..");
            }
            continue;
        }
        push(out, part);
    }
    var body = strings.join(out, "/");
    if absolute {
        body = "/" + body;
    }
    if strings.isEmpty(body) {
        body = ".";
    }
    return drive + body;
}

// `target` written relative to `from`, both read as plain text and cleaned
// first. Both have to be absolute or both relative, else `target` comes back
// cleaned.
func rel(from: str, target: str): str {
    if isAbs(from) != isAbs(target) {
        return clean(target);
    }
    let a = parts(clean(from));
    let b = parts(clean(target));
    var i = 0;
    while i < len(a) && i < len(b) && a[i] == b[i] {
        i += 1;
    }
    var out: [str] = [];
    var j = i;
    while j < len(a) {
        if a[j] != "." {
            push(out, "..");
        }
        j += 1;
    }
    var k = i;
    while k < len(b) {
        push(out, b[k]);
        k += 1;
    }
    if len(out) == 0 {
        return ".";
    }
    return strings.join(out, "/");
}
