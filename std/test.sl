// The `test` module: assertions for `sl test` and for scripts of your own.
//
// A failed assertion prints a line starting `FAIL:` and carries on, so one
// run reports every failure; `sl test` counts those lines to decide whether
// a file passed. Nothing here needs the clock or the filesystem, so it works
// in any host.
//
//     import test;
//     func main() {
//         test.assertEqual(1 + 1, 2, "addition");
//         test.assertNear(0.1 + 0.2, 0.3, 0.0001, "float sum");
//     }

import math;
import strings;

func fail(msg: str) {
    print("FAIL: ${msg}");
}

func assert(cond: bool, msg: str) {
    if !cond {
        print("FAIL: ${msg}");
    }
}

func assertFalse(cond: bool, msg: str) {
    if cond {
        print("FAIL: ${msg}");
    }
}

// Works for ints, floats, strings, bools and the other types `==` accepts.
// The message shows both values.
func assertEqual[T](got: T, want: T, msg: str) {
    if got != want {
        print("FAIL: ${msg} (got ${got}, wanted ${want})");
    }
}

func assertNotEqual[T](got: T, other: T, msg: str) {
    if got == other {
        print("FAIL: ${msg} (both ${got})");
    }
}

// Floats within `tolerance` of each other.
func assertNear(got: float, want: float, tolerance: float, msg: str) {
    if math.abs(got - want) > tolerance {
        print("FAIL: ${msg} (got ${got}, wanted ${want} within ${tolerance})");
    }
}

func assertContains(text: str, part: str, msg: str) {
    if !strings.contains(text, part) {
        print("FAIL: ${msg} (`${text}` does not contain `${part}`)");
    }
}
