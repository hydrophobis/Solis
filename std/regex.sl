// The `regex` module: regular expressions.
//
//     regex.isMatch("^[a-z]+@[a-z]+\\.com$", email)
//     regex.groups("(\\d+)-(\\d+)", "tel 555-0199")      // ["555-0199", "555", "0199"]
//     regex.replace("\\s+", text, " ")
//
// Syntax: literals, `.`, classes `[a-z0-9_]` and `[^...]`, `\d \w \s` and
// their capital negations, `\b \B`, `^ $`, groups `( )` and `(?: )`,
// alternation `|`, and the quantifiers `* + ? {n} {n,} {n,m}`, each with a
// lazy form by adding `?`. A pattern may start with `(?i)` (ignore case,
// ASCII), `(?s)` (`.` matches newline) or `(?m)` (`^` and `$` match at each
// line). There are no backreferences or lookaround.
//
// Matches are leftmost, and among alternatives the first that works wins.
// Positions are in characters, like everywhere else in Solis. A pattern that
// is not valid stops the program with an error naming the problem; test with
// `isValid` when the pattern comes from outside. A match that would take an
// absurd number of steps also stops with an error rather than hanging.

import strings;

// Whether the pattern matches anywhere in the text.
extern func isMatch(pattern: str, text: str): bool;

// The first match, or "" if there is none. Pair with `isMatch` or `index`
// when an empty match is possible.
extern func find(pattern: str, text: str): str;

// Character index of the first match, or -1.
extern func index(pattern: str, text: str): int;

// The first match followed by what each group captured, a group that did
// not take part being "". Empty array when nothing matches.
extern func groups(pattern: str, text: str): [str];

// Every non-overlapping match.
extern func findAll(pattern: str, text: str): [str];

// `groups` for every match.
extern func allGroups(pattern: str, text: str): [[str]];

// Replaces every match. In `with`, `$0` is the whole match, `$1` to `$9` are
// groups, and `$$` is a dollar sign.
extern func replace(pattern: str, text: str, with: str): str;
extern func replaceFirst(pattern: str, text: str, with: str): str;

// Splits around every match. Adjacent matches give empty pieces.
extern func split(pattern: str, text: str): [str];

extern func isValid(pattern: str): bool;
extern func groupCount(pattern: str): int;

// The whole text matches, not just part of it.
func fullMatch(pattern: str, text: str): bool {
    return isMatch("^(?:" + pattern + ")$", text);
}

func count(pattern: str, text: str): int {
    return len(findAll(pattern, text));
}

// `s` with every character that has a meaning in a pattern escaped, to match
// it literally.
func escape(s: str): str {
    var out = "";
    for c in strings.chars(s) {
        if strings.contains("\\.^$|?*+()[]{}-", c) {
            out = out + "\\";
        }
        out = out + c;
    }
    return out;
}
