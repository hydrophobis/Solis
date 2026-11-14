// The `ansi` module: colors and styles for terminals, as escape sequences.
//
//     print(ansi.red("error") + ": " + ansi.bold(message));
//
// Each function wraps its text so the style ends where the text does. Pair
// with `ansi.enabled(os.hasEnv("NO_COLOR"))`-style checks of your own when
// output may go to a file; the functions always emit codes.

import strings;

func esc(): str {
    return strings.fromCode(27);
}

func code(n: int, text: str): str {
    return esc() + "[" + strings.fromInt(n) + "m" + text + esc() + "[0m";
}

func bold(text: str): str { return code(1, text); }
func dim(text: str): str { return code(2, text); }
func italic(text: str): str { return code(3, text); }
func underline(text: str): str { return code(4, text); }

func black(text: str): str { return code(30, text); }
func red(text: str): str { return code(31, text); }
func green(text: str): str { return code(32, text); }
func yellow(text: str): str { return code(33, text); }
func blue(text: str): str { return code(34, text); }
func magenta(text: str): str { return code(35, text); }
func cyan(text: str): str { return code(36, text); }
func white(text: str): str { return code(37, text); }
func gray(text: str): str { return code(90, text); }

// 256-color foreground and background, `n` from 0 to 255.
func color256(n: int, text: str): str {
    return esc() + "[38;5;" + strings.fromInt(n) + "m" + text + esc() + "[0m";
}

func bg256(n: int, text: str): str {
    return esc() + "[48;5;" + strings.fromInt(n) + "m" + text + esc() + "[0m";
}

func rgb(r: int, g: int, b: int, text: str): str {
    return esc() + "[38;2;" + strings.fromInt(r) + ";" + strings.fromInt(g) + ";" + strings.fromInt(b) + "m" + text + esc() + "[0m";
}

// `text` without any escape sequences, for measuring or writing to a file.
func strip(text: str): str {
    var out: [str] = [];
    let cs = strings.chars(text);
    var i = 0;
    while i < len(cs) {
        if cs[i] == esc() && i + 1 < len(cs) && cs[i + 1] == "[" {
            i += 2;
            while i < len(cs) && !strings.contains("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", cs[i]) {
                i += 1;
            }
            i += 1;
        } else {
            push(out, cs[i]);
            i += 1;
        }
    }
    return strings.join(out, "");
}

func clearScreen(): str {
    return esc() + "[2J" + esc() + "[H";
}
