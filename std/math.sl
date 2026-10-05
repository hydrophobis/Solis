// The `math` module.
//
// Compiled into the Solis binary; `import math;` needs no file on disk.
// The `extern` declarations are backed by native code, and everything else is
// ordinary Solis.
//
// Note there is no overloading and no generics, so int and float versions are
// separate functions. `maxf` is the float twin of `max`. That is the honest
// cost of the trim, and it is better than pretending.

// Solis never converts between int and float implicitly, so these are the only
// way across. You will reach for `toFloat` constantly when averaging.

extern func toFloat(n: int): float;
// Truncates toward zero.
extern func toInt(x: float): int;

extern func floor(x: float): int;
extern func ceil(x: float): int;
extern func round(x: float): int;

extern func sqrt(x: float): float;
extern func pow(base: float, exp: float): float;
extern func exp(x: float): float;
extern func ln(x: float): float;
extern func log10(x: float): float;

extern func sin(x: float): float;
extern func cos(x: float): float;
extern func tan(x: float): float;
extern func atan2(y: float, x: float): float;

extern func abs(x: float): float;
extern func absInt(n: int): int;

let PI = 3.141592653589793;
let E = 2.718281828459045;
let TAU = 6.283185307179586;

func min(a: int, b: int): int {
    if a < b {
        return a;
    }
    return b;
}

func max(a: int, b: int): int {
    if a > b {
        return a;
    }
    return b;
}

func minf(a: float, b: float): float {
    if a < b {
        return a;
    }
    return b;
}

func maxf(a: float, b: float): float {
    if a > b {
        return a;
    }
    return b;
}

func clamp(v: int, lo: int, hi: int): int {
    return min(max(v, lo), hi);
}

func clampf(v: float, lo: float, hi: float): float {
    return minf(maxf(v, lo), hi);
}

func sign(n: int): int {
    if n > 0 {
        return 1;
    }
    if n < 0 {
        return 0 - 1;
    }
    return 0;
}

// Linear interpolation. `t` is not clamped.
func lerp(a: float, b: float, t: float): float {
    return a + (b - a) * t;
}

func degrees(radians: float): float {
    return radians * 180.0 / PI;
}

func radians(degrees: float): float {
    return degrees * PI / 180.0;
}
