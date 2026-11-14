// The `toml` module: reads TOML configuration files into a `json.Json` value.
//
//     switch toml.parse(fs.read("solis.toml")) {
//         case Success(cfg): print(json.getStr(cfg, "name", "?"));
//         case Failure(msg): print(msg);
//     }
//
// The result is the same `Json` tree the `json` module uses, so `json.get`,
// `json.getInt` and `json.pretty` all apply, and a TOML file can be turned
// into JSON text by printing it. Tables become objects, arrays of tables
// become arrays of objects, integers and floats are both numbers, and dates
// and times come through as their text. Supported: comments, `[table]` and
// `[[array]]` headers, dotted keys, basic, literal and multi-line strings,
// numbers with `_` and `0x`/`0o`/`0b` forms, `inf` and `nan`, arrays that
// span lines, and inline tables.

import json;
import result;
import strings;
import math;

struct Reader {
    chars: [str];
    pos: int;
    error: str;
}

func fail(r: Reader, message: str) {
    if !strings.isEmpty(r.error) {
        return;
    }
    var line = 1;
    var i = 0;
    while i < r.pos && i < len(r.chars) {
        if r.chars[i] == "\n" {
            line += 1;
        }
        i += 1;
    }
    r.error = "line ${line}: ${message}";
}

func peek(r: Reader): str {
    if r.pos >= len(r.chars) {
        return "";
    }
    return r.chars[r.pos];
}

func peekAt(r: Reader, offset: int): str {
    if r.pos + offset >= len(r.chars) {
        return "";
    }
    return r.chars[r.pos + offset];
}

func failed(r: Reader): bool {
    return !strings.isEmpty(r.error);
}

// Spaces and tabs only.
func skipBlanks(r: Reader) {
    while peek(r) == " " || peek(r) == "\t" {
        r.pos += 1;
    }
}

func skipComment(r: Reader) {
    if peek(r) == "#" {
        while r.pos < len(r.chars) && peek(r) != "\n" {
            r.pos += 1;
        }
    }
}

// Blanks, comments and newlines, for places where a value may continue on
// the next line (inside arrays and inline tables across lines).
func skipSpace(r: Reader) {
    while r.pos < len(r.chars) {
        let c = peek(r);
        if c == " " || c == "\t" || c == "\n" || c == "\r" {
            r.pos += 1;
        } else if c == "#" {
            skipComment(r);
        } else {
            return;
        }
    }
}

func isBare(c: str): bool {
    return strings.len(c) == 1 && strings.contains("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-", c);
}

func hexValue(c: str): int {
    if strings.len(c) != 1 {
        return -1;
    }
    return strings.indexOf("0123456789abcdef", strings.lower(c));
}

func readHex(r: Reader, count: int): int {
    var v = 0;
    var i = 0;
    while i < count {
        let d = hexValue(peek(r));
        if d < 0 {
            fail(r, "bad unicode escape");
            return 0;
        }
        v = v * 16 + d;
        r.pos += 1;
        i += 1;
    }
    return v;
}

// A string body after its opening quote. `quote` is `"` or `'`; `multi` for
// the triple-quoted forms.
func parseStringBody(r: Reader, quote: str, multi: bool): str {
    var out: [str] = [];
    if multi && peek(r) == "\r" && peekAt(r, 1) == "\n" {
        r.pos += 2;
    } else if multi && peek(r) == "\n" {
        r.pos += 1;
    }
    while true {
        if r.pos >= len(r.chars) {
            fail(r, "unterminated string");
            return "";
        }
        let c = peek(r);
        if c == quote {
            if !multi {
                r.pos += 1;
                return strings.join(out, "");
            }
            if peekAt(r, 1) == quote && peekAt(r, 2) == quote {
                r.pos += 3;
                // up to two more quotes can belong to the string
                while peek(r) == quote && strings.len(quote) == 1 {
                    push(out, quote);
                    r.pos += 1;
                    if len(out) > 100000000 {
                        break;
                    }
                }
                return strings.join(out, "");
            }
            push(out, c);
            r.pos += 1;
            continue;
        }
        if c == "\n" && !multi {
            fail(r, "newline in string");
            return "";
        }
        if c != "\\" || quote == "'" {
            push(out, c);
            r.pos += 1;
            continue;
        }
        r.pos += 1;
        let e = peek(r);
        r.pos += 1;
        if e == "n" {
            push(out, "\n");
        } else if e == "t" {
            push(out, "\t");
        } else if e == "r" {
            push(out, "\r");
        } else if e == "b" {
            push(out, strings.fromCode(8));
        } else if e == "f" {
            push(out, strings.fromCode(12));
        } else if e == "e" {
            push(out, strings.fromCode(27));
        } else if e == "\"" || e == "\\" {
            push(out, e);
        } else if e == "u" {
            push(out, strings.fromCode(readHex(r, 4)));
        } else if e == "U" {
            push(out, strings.fromCode(readHex(r, 8)));
        } else if multi && (e == "\n" || e == " " || e == "\t" || e == "\r") {
            // a backslash at the end of a line trims the newline and the
            // blanks that follow
            r.pos -= 1;
            skipBlanks(r);
            if peek(r) == "\r" {
                r.pos += 1;
            }
            if peek(r) != "\n" {
                fail(r, "bad escape");
                return "";
            }
            while peek(r) == "\n" || peek(r) == " " || peek(r) == "\t" || peek(r) == "\r" {
                r.pos += 1;
            }
        } else {
            fail(r, "unknown escape");
            return "";
        }
        if failed(r) {
            return "";
        }
    }
    return "";
}

func parseString(r: Reader): str {
    let q = peek(r);
    if peekAt(r, 1) == q && peekAt(r, 2) == q {
        r.pos += 3;
        return parseStringBody(r, q, true);
    }
    r.pos += 1;
    return parseStringBody(r, q, false);
}

// A key segment: bare or quoted.
func parseKeyPart(r: Reader): str {
    skipBlanks(r);
    let c = peek(r);
    if c == "\"" || c == "'" {
        return parseString(r);
    }
    var out: [str] = [];
    while isBare(peek(r)) {
        push(out, peek(r));
        r.pos += 1;
    }
    if len(out) == 0 {
        fail(r, "expected a key");
        return "";
    }
    return strings.join(out, "");
}

func parseKeyPath(r: Reader): [str] {
    var parts: [str] = [];
    while true {
        push(parts, parseKeyPart(r));
        if failed(r) {
            return parts;
        }
        skipBlanks(r);
        if peek(r) == "." {
            r.pos += 1;
        } else {
            return parts;
        }
    }
    return parts;
}

// The text of a bare value: everything up to a delimiter.
func scanToken(r: Reader): str {
    var out: [str] = [];
    while r.pos < len(r.chars) {
        let c = peek(r);
        if c == "," || c == "]" || c == "}" || c == "\n" || c == "\r" || c == "#" || c == " " || c == "\t" {
            break;
        }
        push(out, c);
        r.pos += 1;
    }
    return strings.join(out, "");
}

func digitsOnly(s: str): bool {
    if strings.isEmpty(s) {
        return false;
    }
    for c in strings.chars(s) {
        if !strings.contains("0123456789", c) {
            return false;
        }
    }
    return true;
}

func parseRadix(s: str, radix: int, digits: str): int {
    var v = 0;
    for c in strings.chars(s) {
        let d = strings.indexOf(digits, strings.lower(c));
        if d < 0 || d >= radix {
            return -1;
        }
        v = v * radix + d;
    }
    return v;
}

func looksLikeDate(t: str): bool {
    return strings.len(t) >= 8 && (strings.at(t, 4) == "-" || strings.at(t, 2) == ":") && digitsOnly(strings.sub(t, 0, 2));
}

func parseScalar(r: Reader): json.Json {
    var token = scanToken(r);
    if strings.isEmpty(token) {
        fail(r, "expected a value");
        return json.Json.Null;
    }
    if token == "true" {
        return json.Json.Bool(true);
    }
    if token == "false" {
        return json.Json.Bool(false);
    }
    // a date, possibly followed by a space and a time
    if looksLikeDate(token) {
        if peek(r) == " " && digitsOnly(peekAt(r, 1)) && digitsOnly(peekAt(r, 2)) && peekAt(r, 3) == ":" {
            r.pos += 1;
            token = token + " " + scanToken(r);
        }
        return json.Json.Str(token);
    }
    var clean = strings.replace(token, "_", "");
    var sign = 1.0;
    var body = clean;
    if strings.startsWith(clean, "+") {
        body = strings.sub(clean, 1, strings.len(clean));
    } else if strings.startsWith(clean, "-") {
        sign = -1.0;
        body = strings.sub(clean, 1, strings.len(clean));
    }
    if body == "inf" {
        return json.Json.Num(sign * math.inf());
    }
    if body == "nan" {
        return json.Json.Num(math.nan());
    }
    if strings.startsWith(body, "0x") && strings.len(body) > 2 {
        let v = parseRadix(strings.sub(body, 2, strings.len(body)), 16, "0123456789abcdef");
        if v >= 0 {
            return json.Json.Num(sign * math.toFloat(v));
        }
    } else if strings.startsWith(body, "0o") && strings.len(body) > 2 {
        let v = parseRadix(strings.sub(body, 2, strings.len(body)), 8, "01234567");
        if v >= 0 {
            return json.Json.Num(sign * math.toFloat(v));
        }
    } else if strings.startsWith(body, "0b") && strings.len(body) > 2 {
        let v = parseRadix(strings.sub(body, 2, strings.len(body)), 2, "01");
        if v >= 0 {
            return json.Json.Num(sign * math.toFloat(v));
        }
    } else if strings.isFloat(body) && strings.len(body) > 0 && strings.contains("0123456789", strings.at(body, 0)) {
        return json.Json.Num(sign * strings.toFloat(body));
    }
    fail(r, "cannot read `${token}` as a value");
    return json.Json.Null;
}

func parseArray(r: Reader): json.Json {
    r.pos += 1;
    var items: [json.Json] = [];
    while true {
        skipSpace(r);
        if peek(r) == "]" {
            r.pos += 1;
            return json.Json.Arr(items);
        }
        push(items, parseValue(r));
        if failed(r) {
            return json.Json.Null;
        }
        skipSpace(r);
        if peek(r) == "," {
            r.pos += 1;
        } else if peek(r) != "]" {
            fail(r, "expected `,` or `]` in array");
            return json.Json.Null;
        }
    }
    return json.Json.Null;
}

// The map at `path` below `table`, creating tables on the way. An array of
// tables is entered at its last element. Sets an error on a clash.
func descend(r: Reader, table: {str: json.Json}, path: [str]): {str: json.Json} {
    var cur = table;
    for name in path {
        if contains(cur, name) {
            switch cur[name] {
                case Obj(fields): cur = fields;
                case Arr(items): {
                    if len(items) == 0 {
                        fail(r, "`${name}` is not a table");
                        return cur;
                    }
                    switch items[len(items) - 1] {
                        case Obj(fields): cur = fields;
                        case Null: fail(r, "`${name}` is not a table");
                        case Bool(b): fail(r, "`${name}` is not a table");
                        case Num(n): fail(r, "`${name}` is not a table");
                        case Str(s): fail(r, "`${name}` is not a table");
                        case Arr(inner): fail(r, "`${name}` is not a table");
                    }
                }
                case Null: fail(r, "`${name}` is not a table");
                case Bool(b): fail(r, "`${name}` is not a table");
                case Num(n): fail(r, "`${name}` is not a table");
                case Str(s): fail(r, "`${name}` is not a table");
            }
            if failed(r) {
                return cur;
            }
        } else {
            var fresh: {str: json.Json} = {};
            cur[name] = json.Json.Obj(fresh);
            cur = fresh;
        }
    }
    return cur;
}

// `key = value` pairs into `table`.
func assign(r: Reader, table: {str: json.Json}, path: [str], value: json.Json) {
    let parent = descend(r, table, path[:len(path) - 1]);
    if failed(r) {
        return;
    }
    let last = path[len(path) - 1];
    if contains(parent, last) {
        fail(r, "key `${last}` is defined twice");
        return;
    }
    parent[last] = value;
}

func parseInlineTable(r: Reader): json.Json {
    r.pos += 1;
    var fields: {str: json.Json} = {};
    skipSpace(r);
    if peek(r) == "}" {
        r.pos += 1;
        return json.Json.Obj(fields);
    }
    while true {
        skipSpace(r);
        let path = parseKeyPath(r);
        if failed(r) {
            return json.Json.Null;
        }
        skipBlanks(r);
        if peek(r) != "=" {
            fail(r, "expected `=`");
            return json.Json.Null;
        }
        r.pos += 1;
        let value = parseValue(r);
        if failed(r) {
            return json.Json.Null;
        }
        assign(r, fields, path, value);
        if failed(r) {
            return json.Json.Null;
        }
        skipSpace(r);
        if peek(r) == "," {
            r.pos += 1;
        } else if peek(r) == "}" {
            r.pos += 1;
            return json.Json.Obj(fields);
        } else {
            fail(r, "expected `,` or `}` in inline table");
            return json.Json.Null;
        }
    }
    return json.Json.Null;
}

func parseValue(r: Reader): json.Json {
    skipBlanks(r);
    let c = peek(r);
    if c == "\"" || c == "'" {
        let s = parseString(r);
        return json.Json.Str(s);
    }
    if c == "[" {
        return parseArray(r);
    }
    if c == "{" {
        return parseInlineTable(r);
    }
    return parseScalar(r);
}

// Reads the rest of a line after a value or header: blanks, a comment, then
// a newline or the end.
func endOfLine(r: Reader) {
    skipBlanks(r);
    skipComment(r);
    if peek(r) == "\r" {
        r.pos += 1;
    }
    if r.pos < len(r.chars) && peek(r) != "\n" {
        fail(r, "unexpected `${peek(r)}`");
        return;
    }
    r.pos += 1;
}

// One document into a tree. Errors name the line.
func parse(text: str): result.Result[json.Json, str] {
    let r = Reader { chars: strings.chars(text), pos: 0, error: "" };
    var root: {str: json.Json} = {};
    var current = root;
    while r.pos < len(r.chars) && !failed(r) {
        skipSpace(r);
        if r.pos >= len(r.chars) {
            break;
        }
        if peek(r) == "[" {
            let isArray = peekAt(r, 1) == "[";
            if isArray {
                r.pos += 2;
            } else {
                r.pos += 1;
            }
            let path = parseKeyPath(r);
            if failed(r) {
                break;
            }
            skipBlanks(r);
            if peek(r) != "]" || (isArray && peekAt(r, 1) != "]") {
                fail(r, "unterminated table header");
                break;
            }
            if isArray {
                r.pos += 2;
            } else {
                r.pos += 1;
            }
            if isArray {
                let parent = descend(r, root, path[:len(path) - 1]);
                if failed(r) {
                    break;
                }
                let last = path[len(path) - 1];
                var fresh: {str: json.Json} = {};
                if contains(parent, last) {
                    switch parent[last] {
                        case Arr(items): push(items, json.Json.Obj(fresh));
                        case Null: fail(r, "`${last}` is not an array of tables");
                        case Bool(b): fail(r, "`${last}` is not an array of tables");
                        case Num(n): fail(r, "`${last}` is not an array of tables");
                        case Str(s): fail(r, "`${last}` is not an array of tables");
                        case Obj(fields): fail(r, "`${last}` is not an array of tables");
                    }
                } else {
                    var items: [json.Json] = [];
                    push(items, json.Json.Obj(fresh));
                    parent[last] = json.Json.Arr(items);
                }
                current = fresh;
            } else {
                current = descend(r, root, path);
            }
            if !failed(r) {
                endOfLine(r);
            }
            continue;
        }
        let path = parseKeyPath(r);
        if failed(r) {
            break;
        }
        skipBlanks(r);
        if peek(r) != "=" {
            fail(r, "expected `=` after the key");
            break;
        }
        r.pos += 1;
        let value = parseValue(r);
        if failed(r) {
            break;
        }
        assign(r, current, path, value);
        if !failed(r) {
            endOfLine(r);
        }
    }
    if failed(r) {
        let bad: result.Result[json.Json, str] = result.Result.Failure(r.error);
        return bad;
    }
    let good: result.Result[json.Json, str] = result.Result.Success(json.Json.Obj(root));
    return good;
}
