// The `flags` module: command-line options.
//
//     let f = flags.parse(os.argv(), ["out", "n"]);
//     let n = flags.getInt(f, "n", 10);
//     for file in f.positional { ... }
//
// Recognised forms: `--name`, `--name=value`, `--name value`, `-x`, `-xyz`
// (three short flags), `-n 5`, and `--` to say the rest are positional. A
// bare `--name` is only given a value from the next argument if its name is
// in `valued`; otherwise it is a switch whose value is "true". Names are
// stored without their dashes.

import strings;

struct Flags {
    named: {str: str};
    positional: [str];
}

func parse(args: [str], valued: [str]): Flags {
    var named: {str: str} = {};
    var positional: [str] = [];
    var i = 0;
    var onlyPositional = false;
    while i < len(args) {
        let a = args[i];
        i += 1;
        if onlyPositional || a == "-" || !strings.startsWith(a, "-") {
            push(positional, a);
            continue;
        }
        if a == "--" {
            onlyPositional = true;
            continue;
        }
        if strings.startsWith(a, "--") {
            let body = strings.sub(a, 2, strings.len(a));
            let eq = strings.indexOf(body, "=");
            if eq >= 0 {
                named[strings.sub(body, 0, eq)] = strings.sub(body, eq + 1, strings.len(body));
            } else if contains(valued, body) && i < len(args) {
                named[body] = args[i];
                i += 1;
            } else {
                named[body] = "true";
            }
            continue;
        }
        // short flags: -abc
        let letters = strings.chars(strings.sub(a, 1, strings.len(a)));
        var k = 0;
        while k < len(letters) {
            let letter = letters[k];
            if contains(valued, letter) {
                if k + 1 < len(letters) {
                    named[letter] = strings.join(letters[k + 1:], "");
                } else if i < len(args) {
                    named[letter] = args[i];
                    i += 1;
                } else {
                    named[letter] = "";
                }
                break;
            }
            named[letter] = "true";
            k += 1;
        }
    }
    return Flags { named: named, positional: positional };
}

func has(f: Flags, name: str): bool {
    return contains(f.named, name);
}

func get(f: Flags, name: str, fallback: str): str {
    if contains(f.named, name) {
        return f.named[name];
    }
    return fallback;
}

func getInt(f: Flags, name: str, fallback: int): int {
    if contains(f.named, name) && strings.isInt(f.named[name]) {
        return strings.toInt(f.named[name]);
    }
    return fallback;
}

func getFloat(f: Flags, name: str, fallback: float): float {
    if contains(f.named, name) && strings.isFloat(f.named[name]) {
        return strings.toFloat(f.named[name]);
    }
    return fallback;
}

// True when the flag is present and not "false" or "0".
func getBool(f: Flags, name: str): bool {
    if !contains(f.named, name) {
        return false;
    }
    let v = strings.lower(f.named[name]);
    return v != "false" && v != "0" && v != "no" && v != "off";
}
