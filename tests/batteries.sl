// The batteries, checked: io, fs, os, time and rand. Outside conformance
// because the Rust reference doesn't have them and shouldn't. A clock and
// a filesystem are the wrong thing to hold a byte-identical oracle to. This
// asserts instead and exits non-zero on failure.
//
//     solis tests/batteries.sl <scratch-dir>
//
// Run via tools/batteries.sh, which makes the scratch directory.

import fs;
import io;
import os;
import rand;
import strings;
import time;

var failures = 0;

func check(what: str, ok: bool) {
    if !ok {
        failures += 1;
        io.errLine("FAIL ${what}");
    }
}

func checkEq(what: str, got: str, want: str) {
    if got != want {
        failures += 1;
        io.errLine("FAIL ${what}: got `${got}`, wanted `${want}`");
    }
}

func checkEqInt(what: str, got: int, want: int) {
    if got != want {
        failures += 1;
        io.errLine("FAIL ${what}: got ${got}, wanted ${want}");
    }
}

// --- os --------------------------------------------------------------------

func testOs(dir: str) {
    let all = os.args();
    check("os.args has the script and the directory", len(all) == 2);
    // The script when interpreted, the image when compiled: both spellings
    // start the same way.
    check("os.args starts with the program", strings.contains(all[0], "batteries.sl"));
    checkEq("os.arg", os.arg(0, "missing"), dir);
    checkEq("os.arg past the end", os.arg(9, "fallback"), "fallback");
    checkEqInt("os.argv", len(os.argv()), 1);

    let p = os.platform();
    check("os.platform is a known name",
          p == "windows" || p == "macos" || p == "linux" || p == "unix");
    check("os.cwd is not empty", strings.len(os.cwd()) > 0);

    check("an unset variable is unset", !os.hasEnv("SOLIS_DEFINITELY_NOT_SET"));
    checkEq("env of an unset variable", os.env("SOLIS_DEFINITELY_NOT_SET"), "");
    checkEq("envOr falls back", os.envOr("SOLIS_DEFINITELY_NOT_SET", "d"), "d");
}

// --- fs --------------------------------------------------------------------

func testFs(dir: str) {
    let f = dir + "/a.txt";

    check("the scratch directory exists", fs.isDir(dir));
    check("a missing file does not exist", !fs.exists(f));
    checkEqInt("size of a missing file", fs.size(f), 0 - 1);
    checkEq("reading a missing file", fs.read(f), "");

    check("write", fs.write(f, "one\ntwo\n"));
    check("exists after write", fs.exists(f));
    check("isFile", fs.isFile(f));
    check("a file is not a directory", !fs.isDir(f));
    checkEqInt("size", fs.size(f), 8);
    checkEq("read", fs.read(f), "one\ntwo\n");

    check("append", fs.append(f, "three\n"));
    checkEq("read after append", fs.read(f), "one\ntwo\nthree\n");

    let ls = fs.lines(f);
    checkEqInt("lines count, trailing newline dropped", len(ls), 3);
    checkEq("lines[0]", ls[0], "one");
    checkEq("lines[2]", ls[2], "three");

    check("writeNew refuses an existing file", !fs.writeNew(f, "x"));
    checkEq("writeNew left it alone", fs.read(f), "one\ntwo\nthree\n");

    // Non-ASCII has to survive the round trip unchanged.
    let u = dir + "/u.txt";
    check("write unicode", fs.write(u, "naïve Grüße ΣΟΦΙΑ"));
    checkEq("read unicode", fs.read(u), "naïve Grüße ΣΟΦΙΑ");
    checkEqInt("unicode length in characters", strings.len(fs.read(u)), 17);

    // An empty file reads as the empty string, which is why `exists` exists.
    let e = dir + "/e.txt";
    check("write empty", fs.write(e, ""));
    check("an empty file still exists", fs.exists(e));
    checkEqInt("an empty file has no bytes", fs.size(e), 0);
    checkEqInt("lines of an empty file", len(fs.lines(e)), 0);

    let sub = dir + "/sub";
    check("mkdir", fs.mkdir(sub));
    check("mkdir made a directory", fs.isDir(sub));
    check("mkdir twice fails", !fs.mkdir(sub));

    let names = fs.list(dir);
    checkEqInt("list", len(names), 4);
    check("list has no dot entries", !hasName(names, "."));
    check("list found the file", hasName(names, "a.txt"));
    check("list found the directory", hasName(names, "sub"));
    checkEqInt("paths", len(fs.paths(dir)), 4);

    let moved = dir + "/b.txt";
    check("rename", fs.rename(f, moved));
    check("the old name is gone", !fs.exists(f));
    checkEq("the contents moved", fs.read(moved), "one\ntwo\nthree\n");

    check("remove the file", fs.remove(moved));
    check("it is gone", !fs.exists(moved));
    check("removing it again fails", !fs.remove(moved));
    check("remove the directory", fs.remove(sub));
    check("remove the rest", fs.remove(u) && fs.remove(e));
    checkEqInt("the scratch directory is empty again", len(fs.list(dir)), 0);
    check("listing something that is not a directory", len(fs.list(f)) == 0);
}

func hasName(names: [str], want: str): bool {
    for n in names {
        if n == want {
            return true;
        }
    }
    return false;
}

// --- time ------------------------------------------------------------------

func testTime() {
    // 2020-01-01 in milliseconds. Any plausible clock is past it.
    check("nowMs is a real epoch time", time.nowMs() > 1577836800000);
    checkEqInt("nowSec agrees with nowMs", time.nowSec(), time.nowMs() / 1000);

    let before = time.monoMs();
    time.sleepMs(20);
    let slept = time.monoMs() - before;
    check("sleepMs slept at least 15ms", slept >= 15);
    check("sleepMs did not sleep for a minute", slept < 60000);

    let stamp = time.iso();
    checkEqInt("iso is 20 characters", strings.len(stamp), 20);
    checkEq("iso ends in Z", strings.at(stamp, 19), "Z");
    checkEq("iso has a T", strings.at(stamp, 10), "T");
    checkEq("today is the front of iso", time.today(), strings.sub(stamp, 0, 10));
}

// --- rand ------------------------------------------------------------------

func testRand() {
    rand.seed(12345);
    let a = rand.below(1000000);
    let b = rand.below(1000000);
    rand.seed(12345);
    checkEqInt("the same seed repeats", rand.below(1000000), a);
    checkEqInt("and keeps repeating", rand.below(1000000), b);
    check("two draws are not the same number", a != b);

    checkEqInt("below(0)", rand.below(0), 0);
    checkEqInt("below(1)", rand.below(1), 0);
    checkEqInt("between with an empty range", rand.between(5, 5), 5);
    checkEqInt("index of nothing", rand.index(0), 0 - 1);

    // Every bucket should be hit, and nothing outside the range should be.
    var counts = [0, 0, 0, 0, 0];
    var i = 0;
    while i < 2000 {
        let r = rand.below(5);
        check("below stays in range", r >= 0 && r < 5);
        counts[r] = counts[r] + 1;
        let f = rand.real();
        check("real stays in range", f >= 0.0 && f < 1.0);
        let t = rand.between(10, 20);
        check("between stays in range", t >= 10 && t < 20);
        i += 1;
    }
    for c in counts {
        // 400 expected per bucket; this is loose enough never to flake and
        // tight enough to catch a generator that is stuck.
        check("every bucket is hit about evenly", c > 250 && c < 550);
    }
}

// --- io --------------------------------------------------------------------

func testIo() {
    // Standard input is empty when this runs, which is the end-of-input case.
    check("eof on empty input", io.eof());
    checkEq("readLine at end of input", io.readLine(), "");
    checkEq("readAll at end of input", io.readAll(), "");
    checkEqInt("lines of nothing", len(io.lines()), 0);
}

func main() {
    let dir = os.arg(0, "");
    if strings.isEmpty(dir) {
        io.errLine("usage: solis tests/batteries.sl <scratch-dir>");
        os.exit(2);
    }

    testOs(dir);
    testFs(dir);
    testTime();
    testRand();
    testIo();

    if failures == 0 {
        print("batteries: ok");
        return;
    }
    io.errLine("batteries: ${failures} failure(s)");
    os.exit(1);
}
