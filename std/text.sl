// The `text` module: laying out and filling in text.
//
//     text.format("hello {name}, you are {age}", values)
//     text.center("title", 20, "=")
//     text.wrap(paragraph, 72)

import strings;

// Centers `s` in `width` characters, padding with `fill` (one character);
// the extra one goes on the right when the padding is odd.
func center(s: str, width: int, fill: str): str {
    let total = width - len(s);
    if total <= 0 {
        return s;
    }
    let left = total / 2;
    return strings.repeat(fill, left) + s + strings.repeat(fill, total - left);
}

// Replaces each `{key}` in `template` with the value for `key`. A key with no
// value is left as written, and `{{` and `}}` give literal braces.
func format(template: str, values: {str: str}): str {
    var out: [str] = [];
    let cs = strings.chars(template);
    var i = 0;
    while i < len(cs) {
        let c = cs[i];
        if c == "{" && i + 1 < len(cs) && cs[i + 1] == "{" {
            push(out, "{");
            i += 2;
        } else if c == "}" && i + 1 < len(cs) && cs[i + 1] == "}" {
            push(out, "}");
            i += 2;
        } else if c == "{" {
            var j = i + 1;
            while j < len(cs) && cs[j] != "}" && cs[j] != "{" {
                j += 1;
            }
            if j < len(cs) && cs[j] == "}" {
                let key = strings.join(cs[i + 1:j], "");
                if contains(values, key) {
                    push(out, values[key]);
                } else {
                    push(out, "{" + key + "}");
                }
                i = j + 1;
            } else {
                push(out, c);
                i += 1;
            }
        } else {
            push(out, c);
            i += 1;
        }
    }
    return strings.join(out, "");
}

// Breaks `s` into lines of at most `width` characters at spaces. A word
// longer than `width` gets a line of its own. Existing newlines are kept.
func wrap(s: str, width: int): str {
    var lines: [str] = [];
    for para in strings.split(s, "\n") {
        var current = "";
        for word in strings.split(para, " ") {
            if strings.isEmpty(word) {
                continue;
            }
            if strings.isEmpty(current) {
                current = word;
            } else if len(current) + 1 + len(word) <= width {
                current = current + " " + word;
            } else {
                push(lines, current);
                current = word;
            }
        }
        push(lines, current);
    }
    return strings.join(lines, "\n");
}

// Puts `prefix` at the start of every line.
func indent(s: str, prefix: str): str {
    var out: [str] = [];
    for line in strings.split(s, "\n") {
        if strings.isEmpty(line) {
            push(out, line);
        } else {
            push(out, prefix + line);
        }
    }
    return strings.join(out, "\n");
}

// The text at least `width` characters wide, aligned left, right or centered.
func alignLeft(s: str, width: int): str {
    return strings.padRight(s, width, " ");
}

func alignRight(s: str, width: int): str {
    return strings.padLeft(s, width, " ");
}
