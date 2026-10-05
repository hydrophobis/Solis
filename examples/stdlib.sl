import math;
import strings;

func main() {
    print("--- math ---");
    // The conversion that Solis forces you to be explicit about.
    let xs = [4, 9, 2, 7, 5];
    let mean = math.toFloat(sum(xs)) / math.toFloat(len(xs));
    print("mean        = ${mean}");
    print("sqrt(2)     = ${math.sqrt(2.0)}");
    print("pow(2, 10)  = ${math.pow(2.0, 10.0)}");
    print("floor(-1.5) = ${math.floor(0.0 - 1.5)}");
    print("round(2.5)  = ${math.round(2.5)}");
    print("clamp(99)   = ${math.clamp(99, 0, 10)}");
    print("lerp        = ${math.lerp(0.0, 10.0, 0.25)}");
    print("PI          = ${math.PI}");
    print("degrees(PI) = ${math.degrees(math.PI)}");

    print("--- strings ---");
    let s = "  Hello, Solis World  ";
    let t = strings.trim(s);
    print("trimmed  = '${t}'");
    print("upper    = ${strings.upper(t)}");
    print("len      = ${strings.len(t)}");
    print("indexOf  = ${strings.indexOf(t, "Solis")}");
    print("sub      = ${strings.sub(t, 7, 12)}");
    print("replace  = ${strings.replace(t, "World", "everyone")}");
    print("reverse  = ${strings.reverse("abcdef")}");
    print("before   = ${strings.before(t, ",")}");
    print("after    = ${strings.after(t, ", ")}");

    let parts = strings.split("a,b,c,d", ",");
    print("split    = ${parts}");
    print("joined   = ${strings.join(parts, " | ")}");
    print("count l  = ${strings.count(t, "l")}");
    print("padded   = '${strings.padLeft("7", 5, "0")}'");

    print("isInt    = ${strings.isInt("42")} ${strings.isInt("4x2")}");
    print("toInt    = ${strings.toInt("-17") + 1}");

    // Non-ASCII: indices are characters, not bytes.
    let u = "naïve";
    print("unicode  = len ${strings.len(u)}, at(2) '${strings.at(u, 2)}'");
}

func sum(xs: [int]): int {
    var t = 0;
    for x in xs { t += x; }
    return t;
}
