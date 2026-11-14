// The `url` module: percent-encoding, query strings and splitting URLs.
//
//     url.encode("a b&c")                    // "a%20b%26c"
//     url.parseQuery("x=1&y=two")            // {"x": "1", "y": "two"}
//     url.parse("https://u@host:8080/p?q=1#top")
//
// Nothing here talks to the network; see `http` for that.

import strings;

func hexDigit(n: int): str {
    return strings.sub("0123456789ABCDEF", n, n + 1);
}

func isUnreserved(c: str): bool {
    return strings.len(c) == 1 && strings.contains("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~", c);
}

// Percent-encodes everything except letters, digits and `-_.~`, byte by
// byte in UTF-8.
func encode(s: str): str {
    var out: [str] = [];
    for c in strings.chars(s) {
        if isUnreserved(c) {
            push(out, c);
        } else {
            for b in strings.bytes(c) {
                push(out, "%" + hexDigit(b / 16) + hexDigit(b % 16));
            }
        }
    }
    return strings.join(out, "");
}

// Reads `%XX` escapes. A malformed escape is left as written. With
// `plusIsSpace`, `+` becomes a space, as in form data.
func decodeWith(s: str, plusIsSpace: bool): str {
    let cs = strings.chars(s);
    var bytes: [int] = [];
    var i = 0;
    while i < len(cs) {
        let c = cs[i];
        if c == "%" && i + 2 < len(cs) {
            let h = strings.indexOf("0123456789abcdef", strings.lower(cs[i + 1]));
            let l = strings.indexOf("0123456789abcdef", strings.lower(cs[i + 2]));
            if h >= 0 && l >= 0 && strings.len(cs[i + 1]) == 1 && strings.len(cs[i + 2]) == 1 {
                push(bytes, h * 16 + l);
                i += 3;
                continue;
            }
        }
        if c == "+" && plusIsSpace {
            push(bytes, 32);
        } else {
            for b in strings.bytes(c) {
                push(bytes, b);
            }
        }
        i += 1;
    }
    return strings.fromBytes(bytes);
}

func decode(s: str): str {
    return decodeWith(s, false);
}

// `a=1&b=two` into a map; later keys win. Keys and values are decoded, and a
// key with no `=` maps to "". A leading `?` is ignored.
func parseQuery(q: str): {str: str} {
    var out: {str: str} = {};
    var text = q;
    if strings.startsWith(text, "?") {
        text = strings.sub(text, 1, strings.len(text));
    }
    if strings.isEmpty(text) {
        return out;
    }
    for pair in strings.split(text, "&") {
        if strings.isEmpty(pair) {
            continue;
        }
        let eq = strings.indexOf(pair, "=");
        if eq < 0 {
            out[decodeWith(pair, true)] = "";
        } else {
            out[decodeWith(strings.sub(pair, 0, eq), true)] = decodeWith(strings.sub(pair, eq + 1, strings.len(pair)), true);
        }
    }
    return out;
}

// A map as `a=1&b=two`, in insertion order, encoded. No leading `?`.
func buildQuery(params: {str: str}): str {
    var parts: [str] = [];
    for k in keys(params) {
        push(parts, encode(k) + "=" + encode(params[k]));
    }
    return strings.join(parts, "&");
}

struct Url {
    scheme: str;
    user: str;
    host: str;
    port: int;         // 0 when the URL does not give one
    path: str;
    query: str;        // without the `?`
    fragment: str;     // without the `#`
}

// Splits `scheme://user@host:port/path?query#fragment`. Every part is
// optional; what is missing comes back empty (port 0). Nothing is decoded.
func parse(text: str): Url {
    var rest = text;
    var fragment = "";
    var query = "";
    var scheme = "";
    var user = "";
    var host = "";
    var port = 0;

    let hash = strings.indexOf(rest, "#");
    if hash >= 0 {
        fragment = strings.sub(rest, hash + 1, strings.len(rest));
        rest = strings.sub(rest, 0, hash);
    }
    let q = strings.indexOf(rest, "?");
    if q >= 0 {
        query = strings.sub(rest, q + 1, strings.len(rest));
        rest = strings.sub(rest, 0, q);
    }
    let sep = strings.indexOf(rest, "://");
    if sep > 0 {
        scheme = strings.lower(strings.sub(rest, 0, sep));
        rest = strings.sub(rest, sep + 3, strings.len(rest));
        var authority = rest;
        var path = "";
        let slash = strings.indexOf(rest, "/");
        if slash >= 0 {
            authority = strings.sub(rest, 0, slash);
            path = strings.sub(rest, slash, strings.len(rest));
        }
        let at = strings.indexOf(authority, "@");
        if at >= 0 {
            user = strings.sub(authority, 0, at);
            authority = strings.sub(authority, at + 1, strings.len(authority));
        }
        host = authority;
        let colon = strings.indexOf(authority, ":");
        if colon >= 0 && !strings.startsWith(authority, "[") {
            let digits = strings.sub(authority, colon + 1, strings.len(authority));
            if strings.isInt(digits) {
                port = strings.toInt(digits);
                host = strings.sub(authority, 0, colon);
            }
        }
        return Url { scheme: scheme, user: user, host: host, port: port, path: path, query: query, fragment: fragment };
    }
    return Url { scheme: "", user: "", host: "", port: 0, path: rest, query: query, fragment: fragment };
}

// The pieces back into text. A zero port is left out.
func format(u: Url): str {
    var out = "";
    if !strings.isEmpty(u.scheme) {
        out = u.scheme + "://";
        if !strings.isEmpty(u.user) {
            out = out + u.user + "@";
        }
        out = out + u.host;
        if u.port != 0 {
            out = out + ":" + strings.fromInt(u.port);
        }
    }
    out = out + u.path;
    if !strings.isEmpty(u.query) {
        out = out + "?" + u.query;
    }
    if !strings.isEmpty(u.fragment) {
        out = out + "#" + u.fragment;
    }
    return out;
}
