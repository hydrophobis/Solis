// The `json` module: parse JSON text into a `Json` value and write one back.
//
//     switch json.parse("{\"hp\": 10}") {
//         case Success(v): print("${json.getInt(v, "hp", 0)}");
//         case Failure(msg): print(msg);
//     }
//
// A `Json` is one of the six JSON shapes. Numbers are all floats, as in JSON
// itself; `asInt` and `getInt` round them for you. Object keys keep the order
// they were written in. A key that appears twice keeps its last value.
//
// The accessors never fail: asking an array for a key, or a missing key for a
// string, gives the fallback (or `Null`), so a chain of `get` calls into
// data of an unexpected shape ends in a default instead of an error.

import result;
import strings;
import math;

enum Json {
    Null,
    Bool(value: bool),
    Num(value: float),
    Str(value: str),
    Arr(items: [Json]),
    Obj(fields: {str: Json}),
}

// --- building ----------------------------------------------------------------

func fromInt(n: int): Json {
    return Json.Num(math.toFloat(n));
}

func emptyArray(): Json {
    var items: [Json] = [];
    return Json.Arr(items);
}

func emptyObject(): Json {
    var fields: {str: Json} = {};
    return Json.Obj(fields);
}

// --- reading -----------------------------------------------------------------

func isNull(v: Json): bool {
    switch v {
        case Null: return true;
        case Bool(b): return false;
        case Num(n): return false;
        case Str(s): return false;
        case Arr(items): return false;
        case Obj(fields): return false;
    }
}

// The value under `key` in an object, or Null if `v` is not an object or has
// no such key.
func get(v: Json, key: str): Json {
    switch v {
        case Obj(fields): {
            if contains(fields, key) {
                return fields[key];
            }
            return Json.Null;
        }
        case Null: return Json.Null;
        case Bool(b): return Json.Null;
        case Num(n): return Json.Null;
        case Str(s): return Json.Null;
        case Arr(list): return Json.Null;
    }
}

// The i'th element of an array, or Null when out of range or not an array.
func at(v: Json, i: int): Json {
    switch v {
        case Arr(items): {
            if i < 0 || i >= len(items) {
                return Json.Null;
            }
            return items[i];
        }
        case Null: return Json.Null;
        case Bool(b): return Json.Null;
        case Num(n): return Json.Null;
        case Str(s): return Json.Null;
        case Obj(fields): return Json.Null;
    }
}

// Elements of an array, keys of an object, otherwise zero.
func size(v: Json): int {
    switch v {
        case Arr(items): return len(items);
        case Obj(fields): return len(fields);
        case Null: return 0;
        case Bool(b): return 0;
        case Num(n): return 0;
        case Str(s): return 0;
    }
}

func has(v: Json, key: str): bool {
    switch v {
        case Obj(fields): return contains(fields, key);
        case Null: return false;
        case Bool(b): return false;
        case Num(n): return false;
        case Str(s): return false;
        case Arr(list): return false;
    }
}

// The keys of an object in order; empty for anything else.
func keysOf(v: Json): [str] {
    var out: [str] = [];
    switch v {
        case Obj(fields): {
            for k in keys(fields) {
                push(out, k);
            }
        }
        case Null: {}
        case Bool(b): {}
        case Num(n): {}
        case Str(s): {}
        case Arr(list): {}
    }
    return out;
}

// The elements of an array; empty for anything else.
func items(v: Json): [Json] {
    var out: [Json] = [];
    switch v {
        case Arr(list): {
            for x in list {
                push(out, x);
            }
        }
        case Null: {}
        case Bool(b): {}
        case Num(n): {}
        case Str(s): {}
        case Obj(fields): {}
    }
    return out;
}

func asStr(v: Json, fallback: str): str {
    switch v {
        case Str(s): return s;
        case Null: return fallback;
        case Bool(b): return fallback;
        case Num(n): return fallback;
        case Arr(list): return fallback;
        case Obj(fields): return fallback;
    }
}

func asFloat(v: Json, fallback: float): float {
    switch v {
        case Num(n): return n;
        case Null: return fallback;
        case Bool(b): return fallback;
        case Str(s): return fallback;
        case Arr(list): return fallback;
        case Obj(fields): return fallback;
    }
}

// A number rounded to the nearest int, or `fallback` for anything else.
func asInt(v: Json, fallback: int): int {
    switch v {
        case Num(n): return math.round(n);
        case Null: return fallback;
        case Bool(b): return fallback;
        case Str(s): return fallback;
        case Arr(list): return fallback;
        case Obj(fields): return fallback;
    }
}

func asBool(v: Json, fallback: bool): bool {
    switch v {
        case Bool(b): return b;
        case Null: return fallback;
        case Num(n): return fallback;
        case Str(s): return fallback;
        case Arr(list): return fallback;
        case Obj(fields): return fallback;
    }
}

// `get` and `asX` in one step.
func getStr(v: Json, key: str, fallback: str): str {
    return asStr(get(v, key), fallback);
}

func getInt(v: Json, key: str, fallback: int): int {
    return asInt(get(v, key), fallback);
}

func getFloat(v: Json, key: str, fallback: float): float {
    return asFloat(get(v, key), fallback);
}

func getBool(v: Json, key: str, fallback: bool): bool {
    return asBool(get(v, key), fallback);
}

// --- writing -----------------------------------------------------------------

func hexDigit(n: int): str {
    return strings.sub("0123456789abcdef", n, n + 1);
}

// `s` as a JSON string literal, quotes included.
func quote(s: str): str {
    var out: [str] = ["\""];
    for c in strings.chars(s) {
        let code = strings.code(c);
        if c == "\"" {
            push(out, "\\\"");
        } else if c == "\\" {
            push(out, "\\\\");
        } else if c == "\n" {
            push(out, "\\n");
        } else if c == "\r" {
            push(out, "\\r");
        } else if c == "\t" {
            push(out, "\\t");
        } else if code == 8 {
            push(out, "\\b");
        } else if code == 12 {
            push(out, "\\f");
        } else if code < 32 {
            push(out, "\\u00" + hexDigit(code / 16) + hexDigit(code % 16));
        } else {
            push(out, c);
        }
    }
    push(out, "\"");
    return strings.join(out, "");
}

func numberText(n: float): str {
    // NaN and the infinities have no JSON spelling.
    if n != n || n > 1.0e300 || n < -1.0e300 {
        return "null";
    }
    if n == math.toFloat(math.floor(n)) && math.abs(n) < 1.0e15 {
        return strings.fromInt(math.toInt(n));
    }
    return strings.fromFloat(n);
}

func write(v: Json, out: [str], indent: int, level: int) {
    switch v {
        case Null: push(out, "null");
        case Bool(b): push(out, strings.fromBool(b));
        case Num(n): push(out, numberText(n));
        case Str(s): push(out, quote(s));
        case Arr(list): {
            if len(list) == 0 {
                push(out, "[]");
                return;
            }
            push(out, "[");
            var first = true;
            for x in list {
                if !first {
                    push(out, ",");
                }
                first = false;
                newline(out, indent, level + 1);
                write(x, out, indent, level + 1);
            }
            newline(out, indent, level);
            push(out, "]");
        }
        case Obj(fields): {
            if len(fields) == 0 {
                push(out, "{}");
                return;
            }
            push(out, "{");
            var first = true;
            for k in keys(fields) {
                if !first {
                    push(out, ",");
                }
                first = false;
                newline(out, indent, level + 1);
                push(out, quote(k));
                if indent > 0 {
                    push(out, ": ");
                } else {
                    push(out, ":");
                }
                write(fields[k], out, indent, level + 1);
            }
            newline(out, indent, level);
            push(out, "}");
        }
    }
}

func newline(out: [str], indent: int, level: int) {
    if indent > 0 {
        push(out, "\n" + strings.repeat(" ", indent * level));
    }
}

// Compact text with no spaces.
func stringify(v: Json): str {
    var out: [str] = [];
    write(v, out, 0, 0);
    return strings.join(out, "");
}

// Indented text, `indent` spaces per level.
func pretty(v: Json, indent: int): str {
    var out: [str] = [];
    write(v, out, indent, 0);
    return strings.join(out, "");
}

// --- parsing -----------------------------------------------------------------

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
    var col = 1;
    var i = 0;
    while i < r.pos && i < len(r.chars) {
        if r.chars[i] == "\n" {
            line += 1;
            col = 1;
        } else {
            col += 1;
        }
        i += 1;
    }
    r.error = "${message} at line ${line}, column ${col}";
}

func peek(r: Reader): str {
    if r.pos >= len(r.chars) {
        return "";
    }
    return r.chars[r.pos];
}

func skipSpace(r: Reader) {
    while r.pos < len(r.chars) {
        let c = r.chars[r.pos];
        if c == " " || c == "\n" || c == "\r" || c == "\t" {
            r.pos += 1;
        } else {
            return;
        }
    }
}

func isDigit(c: str): bool {
    return strings.len(c) == 1 && strings.contains("0123456789", c);
}

func hexValue(c: str): int {
    if strings.len(c) != 1 {
        return -1;
    }
    return strings.indexOf("0123456789abcdef", strings.lower(c));
}

// Four hex digits at the cursor, or -1.
func hex4(r: Reader): int {
    var v = 0;
    var i = 0;
    while i < 4 {
        if r.pos >= len(r.chars) {
            return -1;
        }
        let d = hexValue(r.chars[r.pos]);
        if d < 0 {
            return -1;
        }
        v = v * 16 + d;
        r.pos += 1;
        i += 1;
    }
    return v;
}

func parseString(r: Reader): str {
    r.pos += 1;
    var parts: [str] = [];
    while true {
        if r.pos >= len(r.chars) {
            fail(r, "unterminated string");
            return "";
        }
        let c = r.chars[r.pos];
        r.pos += 1;
        if c == "\"" {
            return strings.join(parts, "");
        }
        if c != "\\" {
            if strings.code(c) < 32 {
                r.pos -= 1;
                fail(r, "control character in string");
                return "";
            }
            push(parts, c);
            continue;
        }
        let e = peek(r);
        r.pos += 1;
        if e == "\"" || e == "\\" || e == "/" {
            push(parts, e);
        } else if e == "n" {
            push(parts, "\n");
        } else if e == "t" {
            push(parts, "\t");
        } else if e == "r" {
            push(parts, "\r");
        } else if e == "b" {
            push(parts, strings.fromCode(8));
        } else if e == "f" {
            push(parts, strings.fromCode(12));
        } else if e == "u" {
            var cp = hex4(r);
            if cp < 0 {
                fail(r, "bad \\u escape");
                return "";
            }
            // A high surrogate has to be followed by \uDC00-\uDFFF.
            if cp >= 55296 && cp < 56320 {
                if peek(r) == "\\" && r.pos + 1 < len(r.chars) && r.chars[r.pos + 1] == "u" {
                    r.pos += 2;
                    let low = hex4(r);
                    if low < 56320 || low > 57343 {
                        fail(r, "bad surrogate pair");
                        return "";
                    }
                    cp = 65536 + (cp - 55296) * 1024 + (low - 56320);
                } else {
                    fail(r, "lone surrogate");
                    return "";
                }
            } else if cp >= 56320 && cp < 57344 {
                fail(r, "lone surrogate");
                return "";
            }
            push(parts, strings.fromCode(cp));
        } else {
            r.pos -= 1;
            fail(r, "unknown escape");
            return "";
        }
    }
    return "";
}

func parseNumber(r: Reader): Json {
    let start = r.pos;
    while r.pos < len(r.chars) {
        let c = r.chars[r.pos];
        if isDigit(c) || c == "-" || c == "+" || c == "." || c == "e" || c == "E" {
            r.pos += 1;
        } else {
            break;
        }
    }
    let text = strings.join(r.chars[start:r.pos], "");
    if !strings.isFloat(text) {
        r.pos = start;
        fail(r, "bad number");
        return Json.Null;
    }
    return Json.Num(strings.toFloat(text));
}

func literal(r: Reader, word: str): bool {
    let cs = strings.chars(word);
    if r.pos + len(cs) > len(r.chars) {
        return false;
    }
    var i = 0;
    while i < len(cs) {
        if r.chars[r.pos + i] != cs[i] {
            return false;
        }
        i += 1;
    }
    r.pos += len(cs);
    return true;
}

func parseArray(r: Reader): Json {
    r.pos += 1;
    var list: [Json] = [];
    skipSpace(r);
    if peek(r) == "]" {
        r.pos += 1;
        return Json.Arr(list);
    }
    while true {
        push(list, parseValue(r));
        if !strings.isEmpty(r.error) {
            return Json.Null;
        }
        skipSpace(r);
        let c = peek(r);
        r.pos += 1;
        if c == "]" {
            return Json.Arr(list);
        }
        if c != "," {
            r.pos -= 1;
            fail(r, "expected `,` or `]`");
            return Json.Null;
        }
    }
    return Json.Null;
}

func parseObject(r: Reader): Json {
    r.pos += 1;
    var fields: {str: Json} = {};
    skipSpace(r);
    if peek(r) == "}" {
        r.pos += 1;
        return Json.Obj(fields);
    }
    while true {
        skipSpace(r);
        if peek(r) != "\"" {
            fail(r, "expected a string key");
            return Json.Null;
        }
        let key = parseString(r);
        if !strings.isEmpty(r.error) {
            return Json.Null;
        }
        skipSpace(r);
        if peek(r) != ":" {
            fail(r, "expected `:`");
            return Json.Null;
        }
        r.pos += 1;
        let value = parseValue(r);
        if !strings.isEmpty(r.error) {
            return Json.Null;
        }
        fields[key] = value;
        skipSpace(r);
        let c = peek(r);
        r.pos += 1;
        if c == "}" {
            return Json.Obj(fields);
        }
        if c != "," {
            r.pos -= 1;
            fail(r, "expected `,` or `}`");
            return Json.Null;
        }
    }
    return Json.Null;
}

func parseValue(r: Reader): Json {
    skipSpace(r);
    let c = peek(r);
    if c == "{" {
        return parseObject(r);
    }
    if c == "[" {
        return parseArray(r);
    }
    if c == "\"" {
        let s = parseString(r);
        return Json.Str(s);
    }
    if c == "-" || isDigit(c) {
        return parseNumber(r);
    }
    if c == "t" && literal(r, "true") {
        return Json.Bool(true);
    }
    if c == "f" && literal(r, "false") {
        return Json.Bool(false);
    }
    if c == "n" && literal(r, "null") {
        return Json.Null;
    }
    if strings.isEmpty(c) {
        fail(r, "unexpected end of input");
    } else {
        fail(r, "unexpected `${c}`");
    }
    return Json.Null;
}

// Parses one JSON document. Anything after it other than whitespace is an
// error. The message names the line and column.
func parse(text: str): result.Result[Json, str] {
    let r = Reader { chars: strings.chars(text), pos: 0, error: "" };
    let value = parseValue(r);
    if strings.isEmpty(r.error) {
        skipSpace(r);
        if r.pos < len(r.chars) {
            fail(r, "unexpected `${r.chars[r.pos]}` after the value");
        }
    }
    if !strings.isEmpty(r.error) {
        let bad: result.Result[Json, str] = result.Result.Failure(r.error);
        return bad;
    }
    let good: result.Result[Json, str] = result.Result.Success(value);
    return good;
}

// The value, or `fallback` if the text does not parse.
func parseOr(text: str, fallback: Json): Json {
    switch parse(text) {
        case Success(v): return v;
        case Failure(message): return fallback;
    }
}
