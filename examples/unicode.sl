// Non-ASCII text. The runtime covers ASCII, Latin-1 Supplement, Latin
// Extended-A, Greek and Cyrillic with range arithmetic instead of tables.
// Every line here must interpret and compile to the same output.

import strings;

func main() {
    let s = "naïve Grüße ΣΟΦΙΑ Привет";

    print("--- indexing is by character, not byte ---");
    print("len     = ${strings.len(s)}");
    print("at(2)   = ${strings.at(s, 2)}");
    print("sub     = ${strings.sub(s, 6, 11)}");
    print("index   = ${strings.indexOf(s, "Привет")}");
    print("reverse = ${strings.reverse("naïve")}");
    print("chars   = ${strings.chars("Grüße")}");
    print("split   = ${strings.split(s, " ")}");

    print("--- case mapping ---");
    print("upper   = ${strings.upper(s)}");
    print("lower   = ${strings.lower(s)}");

    // A code point that uppercases into two.
    print("sharp s = ${strings.upper("straße")}");

    // Capital sigma lowercases to the final form only at the end of a word.
    print("sigma   = ${strings.lower("ΟΔΟΣ")} ${strings.lower("ΣΟΦΙΑ")} ${strings.lower("Σ")}");

    print("latin   = ${strings.upper("łódź čšž")} / ${strings.lower("ŁÓDŹ ČŠŽ")}");
    print("cyril   = ${strings.upper("ёлка мир")} / ${strings.lower("ЁЛКА МИР")}");
    print("nordic  = ${strings.upper("æ ø å þ ð ÿ")} / ${strings.lower("Æ Ø Å Þ Ð Ÿ")}");

    print("--- round trip ---");
    print("stable  = ${strings.lower(strings.upper("Grüße")) == "grüsse"}");
    print("idem    = ${strings.upper(strings.upper(s)) == strings.upper(s)}");
}
