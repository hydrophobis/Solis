// The batteries, checked: io, fs, os, time and rand. Outside conformance
// because the Rust reference doesn't have them and shouldn't. A clock and
// a filesystem are the wrong thing to hold a byte-identical oracle to. This
// asserts instead and exits non-zero on failure.
//
//     solis tests/batteries.sl <scratch-dir>
//
// Run via tools/batteries.sh, which makes the scratch directory.

import ansi;
import base64;
import csv;
import datetime;
import flags;
import fs;
import hash;
import hex;
import http;
import io;
import httpd;
import lists;
import log;
import math;
import net;
import json;
import os;
import path;
import rand;
import regex;
import sqlite;
import strings;
import text;
import time;
import toml;
import url;

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

// --- strings (the additions) -----------------------------------------------

func testStrings() {
    check("isFloat 2.5", strings.isFloat("2.5"));
    check("isFloat -1e3", strings.isFloat(" -1e3 "));
    check("isFloat .5", strings.isFloat(".5"));
    check("isFloat empty", !strings.isFloat(""));
    check("isFloat word", !strings.isFloat("abc"));
    check("isFloat hex", !strings.isFloat("0x10"));
    check("isFloat inf", !strings.isFloat("inf"));
    check("isFloat dot only", !strings.isFloat("."));
    check("isFloat double sign", !strings.isFloat("--1"));
    check("toFloat", strings.toFloat("2.5") == 2.5);
    check("toFloat exponent", strings.toFloat("1e3") == 1000.0);
    check("toFloat bad", strings.toFloat("x") == 0.0);
    checkEqInt("code ascii", strings.code("A"), 65);
    checkEqInt("code multibyte", strings.code("é"), 233);
    checkEqInt("code empty", strings.code(""), -1);
    checkEq("fromCode ascii", strings.fromCode(97), "a");
    checkEqInt("fromCode round trip", strings.code(strings.fromCode(128512)), 128512);
    checkEq("fromCode invalid", strings.fromCode(55296), "");
    checkEq("fromCode negative", strings.fromCode(-1), "");
}

// --- path ------------------------------------------------------------------

func testPath() {
    checkEq("join", path.join("a", "b"), "a/b");
    checkEq("join trailing sep", path.join("a/", "b"), "a/b");
    checkEq("join absolute right", path.join("a", "/b"), "/b");
    checkEq("join root", path.join("/", "b"), "/b");
    checkEq("join empty left", path.join("", "b"), "b");
    checkEq("joinAll", path.joinAll(["a", "b", "c"]), "a/b/c");
    checkEq("base", path.base("a/b/c.txt"), "c.txt");
    checkEq("base trailing", path.base("a/b/"), "b");
    checkEq("base root", path.base("/"), "/");
    checkEq("dir", path.dir("a/b/c.txt"), "a/b");
    checkEq("dir none", path.dir("c.txt"), ".");
    checkEq("dir root child", path.dir("/c"), "/");
    checkEq("ext", path.ext("a/c.tar.gz"), ".gz");
    checkEq("ext dotfile", path.ext(".gitignore"), "");
    checkEq("ext none", path.ext("a/b"), "");
    checkEq("stem", path.stem("a/c.tar.gz"), "c.tar");
    checkEq("withExt", path.withExt("d/f.txt", ".md"), "d/f.md");
    checkEq("withExt bare", path.withExt("f.txt", ".md"), "f.md");
    check("isAbs unix", path.isAbs("/x"));
    check("isAbs drive", path.isAbs("C:/x"));
    check("isAbs relative", !path.isAbs("x/y"));
    checkEq("clean dots", path.clean("a/./b/../c//d/"), "a/c/d");
    checkEq("clean leading ..", path.clean("../../a"), "../../a");
    checkEq("clean root ..", path.clean("/../a"), "/a");
    checkEq("clean to nothing", path.clean("a/.."), ".");
    checkEq("clean backslashes", path.clean("C:\\Users\\me\\..\\you"), "C:/Users/you");
    checkEq("rel down", path.rel("/a/b", "/a/c/d"), "../c/d");
    checkEq("rel same", path.rel("/a", "/a"), ".");
    checkEq("rel up", path.rel("/a/b/c", "/a"), "../..");
}

// --- json ------------------------------------------------------------------

func parsed(text: str): json.Json {
    return json.parseOr(text, json.Json.Str("PARSE FAILED"));
}

func parseFails(text: str): bool {
    switch json.parse(text) {
        case Success(v): return false;
        case Failure(m): return true;
    }
}

func testJson() {
    let v = parsed("{\"name\": \"ada\", \"hp\": 10, \"tags\": [\"a\", 1.5, true, null], \"in\": {\"x\": -2e2}}");
    checkEq("getStr", json.getStr(v, "name", "?"), "ada");
    checkEqInt("getInt", json.getInt(v, "hp", 0), 10);
    checkEqInt("nested", json.getInt(json.get(v, "in"), "x", 0), -200);
    checkEqInt("array size", json.size(json.get(v, "tags")), 4);
    check("array float", json.asFloat(json.at(json.get(v, "tags"), 1), 0.0) == 1.5);
    check("array bool", json.asBool(json.at(json.get(v, "tags"), 2), false));
    check("array null", json.isNull(json.at(json.get(v, "tags"), 3)));
    check("missing key is null", json.isNull(json.get(v, "nope")));
    checkEq("missing fallback", json.getStr(v, "nope", "dflt"), "dflt");
    checkEq("wrong type fallback", json.getStr(v, "hp", "dflt"), "dflt");
    check("out of range", json.isNull(json.at(json.get(v, "tags"), 9)));
    check("has", json.has(v, "hp") && !json.has(v, "zz"));
    checkEq("key order", strings.join(json.keysOf(v), ","), "name,hp,tags,in");

    checkEq("round trip compact",
            json.stringify(v), "{\"name\":\"ada\",\"hp\":10,\"tags\":[\"a\",1.5,true,null],\"in\":{\"x\":-200}}");
    checkEq("pretty",
            json.pretty(parsed("{\"a\":[1,2],\"b\":{}}"), 2),
            "{\n  \"a\": [\n    1,\n    2\n  ],\n  \"b\": {}\n}");
    checkEq("empty containers", json.stringify(parsed("[[],{}]")), "[[],{}]");

    checkEq("escapes in", json.getStr(parsed("{\"s\":\"a\\nb\\t\\\"q\\\\\"}"), "s", ""), "a\nb\t\"q\\");
    checkEq("escapes out", json.quote("a\nb\"c\\"), "\"a\\nb\\\"c\\\\\"");
    checkEq("control out", json.quote(strings.fromCode(1)), "\"\\u0001\"");
    checkEq("unicode escape", json.asStr(parsed("\"\\u00e9\""), ""), "é");
    checkEqInt("surrogate pair", strings.code(json.asStr(parsed("\"\\ud83d\\ude00\""), "")), 128512);
    checkEq("raw unicode out", json.stringify(json.Json.Str("é")), "\"é\"");
    checkEq("integers print bare", json.stringify(parsed("[3, 3.0, 0.5, -7]")), "[3,3,0.5,-7]");
    checkEqInt("asInt rounds", json.asInt(parsed("2.6"), 0), 3);
    checkEq("duplicate key last wins", json.getStr(parsed("{\"k\":\"a\",\"k\":\"b\"}"), "k", ""), "b");
    checkEq("whitespace", json.stringify(parsed(" \n[ 1 ,\t2 ]\r\n")), "[1,2]");
    check("fromInt", json.stringify(json.fromInt(42)) == "42");

    var obj = json.emptyObject();
    switch obj {
        case Obj(fields): {
            fields["n"] = json.fromInt(1);
            fields["list"] = json.emptyArray();
        }
        case Null: {}
        case Bool(b): {}
        case Num(n): {}
        case Str(s): {}
        case Arr(list): {}
    }
    checkEq("built", json.stringify(obj), "{\"n\":1,\"list\":[]}");

    for bad in ["", "{", "[1,]", "{\"a\" 1}", "tru", "[1] x", "\"abc", "{\"a\":1,}", "[1 2]", "\"\\q\"",
                "\"\\ud83d\"", "-", "{1:2}", "nul"] {
        check("rejects `${bad}`", parseFails(bad));
    }
    switch json.parse("[1,\n 2,\n x]") {
        case Success(w): check("error position", false);
        case Failure(m): checkEq("error position", m, "unexpected `x` at line 3, column 2");
    }
}

// --- os.output -------------------------------------------------------------

func testProcess() {
    if os.isWindows() {
        return;
    }
    checkEq("output", os.output("printf 'a\\nb'"), "a\nb");
    checkEqInt("status 0", os.lastStatus(), 0);
    checkEq("output with failure", os.output("echo x; exit 3"), "x\n");
    checkEqInt("status 3", os.lastStatus(), 3);
}

// --- hash, hex, base64 ------------------------------------------------------

func testEncodings() {
    checkEq("md5 empty", hash.md5(""), "d41d8cd98f00b204e9800998ecf8427e");
    checkEq("md5", hash.md5("hello"), "5d41402abc4b2a76b9719d911017c592");
    checkEq("sha1", hash.sha1("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
    checkEq("sha256", hash.sha256("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    checkEq("sha256 empty", hash.sha256(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    let long = strings.repeat("a", 1000);
    checkEq("sha256 long", hash.sha256(long), "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
    checkEq("md5 long", hash.md5(long), "cabe45dcc9ae5b66ba86600cca6b8ba8");
    checkEq("sha1 long", hash.sha1(long), "291e9a6c66994949b57ba5e650361e98fc36b1ba");
    checkEq("hmac", hash.hmacSha256("key", "The quick brown fox jumps over the lazy dog"),
            "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
    checkEqInt("crc32", hash.crc32("123456789"), 3421780262);
    checkEqInt("fnv1a", hash.fnv1a("hello"), -6615550055289275125);

    checkEq("hex encode", hex.encode("Hi"), "4869");
    checkEq("hex decode", hex.decode("4869"), "Hi");
    checkEq("hex decode upper", hex.encode(hex.decode("48FF")), "48ff");
    checkEq("hex odd", hex.decode("486"), "");
    check("hex isValid", hex.isValid("00ff") && !hex.isValid("0g") && !hex.isValid("abc"));
    checkEq("hex multibyte", hex.encode("é"), "c3a9");

    checkEq("b64 Man", base64.encode("Man"), "TWFu");
    checkEq("b64 Ma", base64.encode("Ma"), "TWE=");
    checkEq("b64 M", base64.encode("M"), "TQ==");
    checkEq("b64 empty", base64.encode(""), "");
    checkEq("b64 decode", base64.decode("TWFu"), "Man");
    checkEq("b64 decode padded", base64.decode("TWE="), "Ma");
    checkEq("b64 decode unpadded", base64.decode("TWE"), "Ma");
    checkEq("b64 decode spaces", base64.decode("TW Fu\n"), "Man");
    let raw = strings.fromBytes([251, 255, 254]);
    checkEq("b64 standard alphabet", base64.encode(raw), "+//+");
    checkEq("b64 url alphabet", base64.encodeUrl(raw), "-__-");
    check("b64 url decodes", base64.decode("-__-") == raw && base64.decode("+//+") == raw);
    check("b64 isValid", base64.isValid("TWFu") && !base64.isValid("T!Fu") && !base64.isValid("T"));
    checkEq("b64 round trip", base64.decode(base64.encode("héllo wörld")), "héllo wörld");
    checkEqInt("bytes", len(strings.bytes("aé")), 3);
    checkEq("fromBytes", strings.fromBytes([104, 105]), "hi");
}

// --- regex -----------------------------------------------------------------

func testRegex() {
    check("isMatch", regex.isMatch("^[a-z]+@[a-z]+\\.com$", "bob@site.com"));
    check("isMatch no", !regex.isMatch("^[a-z]+@[a-z]+\\.com$", "bob@site.org"));
    checkEq("find", regex.find("\\d+", "ab 123 cd 45"), "123");
    checkEq("find none", regex.find("\\d+", "abc"), "");
    checkEqInt("index", regex.index("b+", "aabbb"), 2);
    checkEqInt("index chars", regex.index("é", "aaé"), 2);
    checkEqInt("index none", regex.index("z", "abc"), -1);
    checkEq("groups", strings.join(regex.groups("(\\d+)-(\\d+)", "tel 555-0199"), "|"), "555-0199|555|0199");
    checkEq("unmatched group is empty", strings.join(regex.groups("(a)|(b)", "b"), "|"), "b||b");
    checkEqInt("groups none", len(regex.groups("x", "abc")), 0);
    checkEq("findAll", strings.join(regex.findAll("\\d+", "a1b22c333"), ","), "1,22,333");
    checkEqInt("allGroups", len(regex.allGroups("(a)(b)?", "ab a")), 2);
    checkEq("replace", regex.replace("\\s+", "a  b\t\nc", " "), "a b c");
    checkEq("replace groups", regex.replace("(\\w+)@(\\w+)", "bob@site", "$2:$1"), "site:bob");
    checkEq("replace dollar", regex.replace("x", "axb", "$$"), "a$b");
    checkEq("replaceFirst", regex.replaceFirst("a", "aaa", "b"), "baa");
    checkEq("split", strings.join(regex.split(",\\s*", "a, b,c,  d"), "|"), "a|b|c|d");
    checkEq("empty matches", regex.replace("x*", "abxd", "-"), "-a-b--d-");
    check("lazy", regex.find("a+?", "aaa") == "a" && regex.find("a+", "aaa") == "aaa");
    check("alternation order", regex.find("ab|a", "ab") == "ab" && regex.find("a|ab", "ab") == "a");
    check("counted", regex.find("a{2,3}", "aaaa") == "aaa" && !regex.isMatch("^a{2}$", "aaa"));
    check("classes", regex.find("[^a-c]+", "abxyzc") == "xyz" && regex.isMatch("[\\d.]+", "3.14"));
    check("word boundary", regex.count("\\bcat\\b", "cat concat cat.") == 2);
    check("ignore case", regex.isMatch("(?i)hello", "HeLLo") && !regex.isMatch("hello", "HELLO"));
    check("dotall", regex.isMatch("(?s)a.b", "a\nb") && !regex.isMatch("a.b", "a\nb"));
    check("multiline", regex.count("(?m)^\\w", "ab\ncd\nef") == 3 && regex.count("^\\w", "ab\ncd") == 1);
    check("non-capturing", regex.groupCount("(?:a)(b)") == 1);
    check("fullMatch", regex.fullMatch("a|ab", "ab") && !regex.fullMatch("a", "ab"));
    check("isValid", regex.isValid("a(b|c)*") && !regex.isValid("a(") && !regex.isValid("*a") && !regex.isValid("[a"));
    checkEq("escape", regex.escape("a.b*c"), "a\\.b\\*c");
    check("escaped literal", regex.isMatch(regex.escape("1+1=2"), "so 1+1=2 yes"));
    check("unicode", regex.find(".", "éa") == "é");
    check("nullable loop", regex.isMatch("^(a*)*$", "aaaa") && regex.isMatch("^(a|b*)*c$", "abbac"));
}

// --- csv -------------------------------------------------------------------

func testCsv() {
    let rows = csv.parse("name,age\n\"Lee, Ann\",31\r\n\"say \"\"hi\"\"\",\n");
    checkEqInt("csv rows", len(rows), 3);
    checkEq("csv header", strings.join(rows[0], "|"), "name|age");
    checkEq("csv quoted comma", rows[1][0], "Lee, Ann");
    checkEq("csv crlf", rows[1][1], "31");
    checkEq("csv doubled quote", rows[2][0], "say \"hi\"");
    checkEqInt("csv empty last cell", len(rows[2]), 2);
    checkEq("csv empty last cell value", rows[2][1], "");
    checkEq("csv multiline cell", csv.parse("\"a\nb\",c")[0][0], "a\nb");
    checkEqInt("csv blank text", len(csv.parse("")), 0);
    checkEqInt("csv blank lines skipped", len(csv.parse("a\n\nb\n")), 2);
    checkEq("csv sep", csv.parseWith("a;b;c", ";")[0][1], "b");
    let recs = csv.records("id,name\n1,ada\n2,bo\n");
    checkEqInt("records", len(recs), 2);
    checkEq("record field", recs[1]["name"], "bo");
    checkEq("format", csv.format([["a", "b,c"], ["say \"x\"", "y"]]), "a,\"b,c\"\n\"say \"\"x\"\"\",y\n");
    let again = csv.parse(csv.format([["a", "b\nc", "d\"e"], ["", "x", " y"]]));
    checkEq("round trip", again[0][1] + "|" + again[0][2] + "|" + again[1][2], "b\nc|d\"e| y");
}

// --- url -------------------------------------------------------------------

func testUrl() {
    checkEq("encode", url.encode("a b&c/é"), "a%20b%26c%2F%C3%A9");
    checkEq("encode unreserved", url.encode("A-z_0.9~"), "A-z_0.9~");
    checkEq("decode", url.decode("a%20b%26c%2F%C3%A9"), "a b&c/é");
    checkEq("decode lower hex", url.decode("%c3%a9"), "é");
    checkEq("decode bad", url.decode("100%"), "100%");
    checkEq("decode bad digits", url.decode("%zz%4"), "%zz%4");
    checkEq("decode plus stays", url.decode("a+b"), "a+b");
    let q = url.parseQuery("?x=1&y=two%20words&flag&z=a+b");
    checkEq("query x", q["x"], "1");
    checkEq("query decoded", q["y"], "two words");
    checkEq("query flag", q["flag"], "");
    checkEq("query plus", q["z"], "a b");
    checkEq("buildQuery", url.buildQuery(q), "x=1&y=two%20words&flag=&z=a%20b");
    let u = url.parse("https://user@Example.com:8080/a/b?x=1&y=2#top");
    checkEq("scheme", u.scheme, "https");
    checkEq("user", u.user, "user");
    checkEq("host", u.host, "Example.com");
    checkEqInt("port", u.port, 8080);
    checkEq("path", u.path, "/a/b");
    checkEq("query", u.query, "x=1&y=2");
    checkEq("fragment", u.fragment, "top");
    checkEq("format url", url.format(u), "https://user@Example.com:8080/a/b?x=1&y=2#top");
    let bare = url.parse("http://host");
    checkEq("no path", bare.path + "|" + bare.host, "|host");
    checkEqInt("no port", bare.port, 0);
    let rel = url.parse("/just/a/path?q=1");
    checkEq("relative", rel.path + "|" + rel.query + "|" + rel.scheme, "/just/a/path|q=1|");
}

// --- datetime ---------------------------------------------------------------

func checkTime(what: str, d: datetime.DateTime, want: str) {
    checkEq(what, datetime.iso(d), want);
}

func testDatetime() {
    let t = datetime.fromUnix(1700000000);
    checkTime("fromUnix", t, "2023-11-14T22:13:20Z");
    checkEqInt("weekday", t.weekday, 2);
    checkEq("format all", datetime.format(t, "%A %a %B %b %j %y %e %I %p %F %T %%"),
            "Tuesday Tue November Nov 318 23 14 10 PM 2023-11-14 22:13:20 %");
    checkEqInt("round trip", datetime.toUnix(t), 1700000000);
    checkTime("epoch", datetime.fromUnix(0), "1970-01-01T00:00:00Z");
    checkEqInt("epoch weekday", datetime.fromUnix(0).weekday, 4);
    checkTime("before epoch", datetime.fromUnix(-1), "1969-12-31T23:59:59Z");
    checkEqInt("before epoch weekday", datetime.fromUnix(-1).weekday, 3);
    checkEqInt("leap day", datetime.toUnix(datetime.make(2024, 2, 29, 12, 0, 0)), 1709208000);
    checkEqInt("1900", datetime.toUnix(datetime.make(1900, 3, 1, 0, 0, 0)), -2203891200);
    checkTime("carry month", datetime.make(2023, 14, 1, 0, 0, 0), "2024-02-01T00:00:00Z");
    checkTime("carry day", datetime.make(2023, 1, 32, 0, 0, 0), "2023-02-01T00:00:00Z");
    checkTime("carry second", datetime.make(2023, 1, 1, 0, 0, 75), "2023-01-01T00:01:15Z");
    checkTime("addDays", datetime.addDays(datetime.fromUnix(0), 365), "1971-01-01T00:00:00Z");
    checkTime("addMonths clamps", datetime.addMonths(datetime.make(2024, 1, 31, 0, 0, 0), 1), "2024-02-29T00:00:00Z");
    checkTime("addMonths back", datetime.addMonths(datetime.make(2024, 3, 15, 0, 0, 0), -4), "2023-11-15T00:00:00Z");
    check("leap years", datetime.isLeap(2000) && datetime.isLeap(2024) && !datetime.isLeap(1900) && !datetime.isLeap(2023));
    checkEqInt("days in Feb", datetime.daysInMonth(2024, 2), 29);
    checkEqInt("dayOfYear", datetime.dayOfYear(datetime.make(2024, 12, 31, 0, 0, 0)), 366);
    checkEqInt("diff", datetime.diffSeconds(datetime.fromUnix(100), datetime.fromUnix(160)), 60);

    switch datetime.parse("2023-11-14T22:13:20Z") {
        case Success(d): checkEqInt("parse", datetime.toUnix(d), 1700000000);
        case Failure(m): check("parse: ${m}", false);
    }
    switch datetime.parse("2023-11-14 22:13:20.789+02:00") {
        case Success(d): checkTime("parse offset", d, "2023-11-14T20:13:20Z");
        case Failure(m): check("parse offset: ${m}", false);
    }
    switch datetime.parse("2024-02-29") {
        case Success(d): checkTime("parse date", d, "2024-02-29T00:00:00Z");
        case Failure(m): check("parse date: ${m}", false);
    }
    for bad in ["", "2023-13-01", "2023-02-30", "2023-1-1", "2023-11-14T25:00", "yesterday", "2023-11-14T22:13:20 PST"] {
        switch datetime.parse(bad) {
            case Success(d): check("rejects `${bad}`", false);
            case Failure(m): check("rejects `${bad}`", true);
        }
    }
}

// --- lists -----------------------------------------------------------------

func testLists() {
    checkEq("concat", strings.join(lists.concat(["a"], ["b", "c"]), ""), "abc");
    checkEqInt("flatten", len(lists.flatten([[1, 2], [3], [4, 5]])), 5);
    checkEq("reversed", strings.join(lists.reversed(["a", "b", "c"]), ""), "cba");
    checkEqInt("repeat", len(lists.repeat(7, 4)), 4);
    let c = lists.chunks([1, 2, 3, 4, 5], 2);
    checkEqInt("chunks count", len(c), 3);
    checkEqInt("chunks last", len(c[2]), 1);
    checkEqInt("firstOr", lists.firstOr([9, 8], 0), 9);
    var none: [int] = [];
    checkEqInt("firstOr empty", lists.firstOr(none, 0), 0);
    checkEqInt("lastOr", lists.lastOr([9, 8], 0), 8);
    checkEqInt("sumInt", lists.sumInt([1, 2, 3]), 6);
    check("sumFloat", lists.sumFloat([0.5, 0.25]) == 0.75);
    checkEqInt("minInt", lists.minInt([3, 1, 2], 0), 1);
    checkEqInt("maxInt", lists.maxInt([3, 1, 2], 0), 3);
    checkEqInt("maxInt empty", lists.maxInt(none, -1), -1);
    check("mean", lists.mean([1.0, 2.0, 6.0]) == 3.0);
    checkEq("uniqueInts", strings.join(strings.split(strings.fromInt(len(lists.uniqueInts([1, 2, 1, 3, 2]))), ","), ""), "3");
    checkEqInt("uniqueStrs", len(lists.uniqueStrs(["a", "b", "a"])), 2);
    let original = [3, 1, 2];
    let sorted = lists.sortedInts(original);
    checkEqInt("sorted first", sorted[0], 1);
    checkEqInt("sorted keeps original", original[0], 3);
    checkEq("sortedStrs", strings.join(lists.sortedStrs(["b", "c", "a"]), ""), "abc");
}

// --- flags -----------------------------------------------------------------

func testFlags() {
    let f = flags.parse(["--name=ada", "-v", "--out", "x.txt", "file1", "-n5", "--dry", "--", "--notflag", "-"],
                        ["out", "n"]);
    checkEq("flag =", flags.get(f, "name", ""), "ada");
    check("short switch", flags.getBool(f, "v"));
    checkEq("valued flag", flags.get(f, "out", ""), "x.txt");
    checkEqInt("short valued", flags.getInt(f, "n", 0), 5);
    check("long switch", flags.getBool(f, "dry"));
    checkEqInt("positional", len(f.positional), 3);
    checkEq("after --", f.positional[1], "--notflag");
    checkEq("lone dash", f.positional[2], "-");
    check("absent", !flags.has(f, "zzz") && !flags.getBool(f, "zzz"));
    checkEq("fallback", flags.get(f, "zzz", "dflt"), "dflt");
    let g = flags.parse(["-abc", "--x=false", "--y=2.5"], []);
    check("combined", flags.getBool(g, "a") && flags.getBool(g, "b") && flags.getBool(g, "c"));
    check("false value", !flags.getBool(g, "x"));
    check("float", flags.getFloat(g, "y", 0.0) == 2.5);
}

// --- math and strings additions -------------------------------------------------

func close(a: float, b: float): bool {
    return math.abs(a - b) < 0.000001;
}

func testMathExtras() {
    check("asin", close(math.asin(1.0), math.PI / 2.0));
    check("acos", close(math.acos(1.0), 0.0));
    check("atan", close(math.atan(1.0), math.PI / 4.0));
    check("sinh", close(math.sinh(0.0), 0.0));
    check("cosh", close(math.cosh(0.0), 1.0));
    check("tanh", close(math.tanh(0.0), 0.0));
    check("log2", close(math.log2(8.0), 3.0));
    check("cbrt", close(math.cbrt(27.0), 3.0));
    check("hypot", close(math.hypot(3.0, 4.0), 5.0));
    check("trunc", math.trunc(-2.7) == -2.0);
    check("mod", close(math.mod(7.5, 2.0), 1.5) && math.mod(1.0, 0.0) == 0.0);
    check("nan", math.isNaN(math.nan()) && !math.isNaN(1.0) && math.nan() != math.nan());
    check("inf", math.isInf(math.inf()) && math.isInf(-math.inf()) && !math.isInf(1.0e300));
}

func testStringsExtras() {
    checkEqInt("lines", len(strings.lines("a\r\nb\n")), 2);
    checkEqInt("lines empty", len(strings.lines("")), 0);
    checkEq("trimStart", strings.trimStart("  a b  "), "a b  ");
    checkEq("trimEnd", strings.trimEnd("  a b  "), "  a b");
    check("isBlank", strings.isBlank(" \t\n") && !strings.isBlank(" a "));
    checkEq("capitalize", strings.capitalize("hello"), "Hello");
    checkEq("removePrefix", strings.removePrefix("foobar", "foo"), "bar");
    checkEq("removeSuffix", strings.removeSuffix("foobar", "bar"), "foo");
    checkEq("removePrefix absent", strings.removePrefix("foobar", "x"), "foobar");
    check("equalsIgnoreCase", strings.equalsIgnoreCase("Hello", "hELLO"));
}

// --- log and http (no network) ------------------------------------------------

func testLogAndHttp() {
    log.setLevel(log.OFF);
    log.info("this goes nowhere");
    checkEqInt("log level", log.getLevel(), 4);
    check("setLevelByName", log.setLevelByName("WARN") && log.getLevel() == 2 && !log.setLevelByName("loud"));
    log.setLevel(log.INFO);

    var quoted = "'it'\\''s'";
    if os.isWindows() {
        quoted = "\"it's\"";
    }
    checkEq("shellQuote", http.shellQuote("it's"), quoted);
    let h = http.parseHeaders("HTTP/1.1 301 Moved\r\nLocation: /x\r\n\r\nHTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nX-A:  b \r\n\r\n");
    checkEq("headers last response", h["content-type"], "text/plain");
    checkEq("headers trimmed", h["x-a"], "b");
    check("headers drop redirect", !contains(h, "location"));
    let down = http.get("http://127.0.0.1:1/");
    checkEqInt("connection refused status", down.status, 0);
    check("connection refused not ok", !http.ok(down) && !strings.isEmpty(down.error));
}

// --- text, ansi, toml ------------------------------------------------------------

func testTextAndAnsi() {
    checkEq("fixed", strings.fixed(3.14159, 2), "3.14");
    checkEq("fixed round", strings.fixed(2.5, 0), "2");
    checkEq("fixed pad", strings.fixed(2.0, 3), "2.000");
    checkEq("fixed negative", strings.fixed(-0.5, 1), "-0.5");
    checkEq("center", text.center("ab", 7, "*"), "**ab***");
    checkEq("center too narrow", text.center("abcdef", 3, "*"), "abcdef");
    var values: {str: str} = {};
    values["name"] = "Ada";
    values["n"] = "3";
    checkEq("format", text.format("hi {name}, {n} left {{literal}} {missing}", values), "hi Ada, 3 left {literal} {missing}");
    checkEq("format unterminated", text.format("a {b", values), "a {b");
    checkEq("wrap", text.wrap("the quick brown fox jumps", 10), "the quick\nbrown fox\njumps");
    checkEq("wrap long word", text.wrap("a verylongword b", 5), "a\nverylongword\nb");
    checkEq("indent", text.indent("a\n\nb", "  "), "  a\n\n  b");
    checkEq("align", text.alignRight("7", 3) + "|" + text.alignLeft("7", 3), "  7|7  ");
    checkEq("ansi wraps", ansi.strip(ansi.red("hi")), "hi");
    check("ansi codes", strings.startsWith(ansi.red("x"), strings.fromCode(27) + "[31m") && strings.endsWith(ansi.bold("x"), "[0m"));
    checkEq("strip many", ansi.strip(ansi.bold(ansi.rgb(1, 2, 3, "a")) + "b"), "ab");
}

func tomlText(src: str): str {
    switch toml.parse(src) {
        case Success(v): return json.stringify(v);
        case Failure(m): return "ERROR " + m;
    }
}

func testToml() {
    checkEq("scalars", tomlText("a = 1\nb = \"two\"\nc = 3.5\nd = true\n"), "{\"a\":1,\"b\":\"two\",\"c\":3.5,\"d\":true}");
    checkEq("tables", tomlText("[s]\nhost = \"h\" # note\n[s.t]\non = false\n"), "{\"s\":{\"host\":\"h\",\"t\":{\"on\":false}}}");
    checkEq("array of tables", tomlText("[[i]]\nn = 1\n[[i]]\nn = 2\n[i.x]\ny = 3\n"), "{\"i\":[{\"n\":1},{\"n\":2,\"x\":{\"y\":3}}]}");
    checkEq("arrays", tomlText("a = [1,\n 2, # c\n]\nb = [[1], []]\n"), "{\"a\":[1,2],\"b\":[[1],[]]}");
    checkEq("inline table", tomlText("p = { x = 1, y = { z = 2 } }\n"), "{\"p\":{\"x\":1,\"y\":{\"z\":2}}}");
    checkEq("dotted keys", tomlText("a.b = 1\na.c = 2\n"), "{\"a\":{\"b\":1,\"c\":2}}");
    checkEq("strings", tomlText("a = 'raw \\n'\nb = \"t\\tq\\\"\\u00e9\"\n"), "{\"a\":\"raw \\\\n\",\"b\":\"t\\tq\\\"é\"}");
    checkEq("multiline", tomlText("t = \"\"\"\nl1\nl2 \\\n   l3\"\"\"\n"), "{\"t\":\"l1\\nl2 l3\"}");
    checkEq("numbers", tomlText("h = 0xFF\no = 0o17\nb = 0b101\nn = -17\nu = 1_000\ne = 1e3\n"), "{\"h\":255,\"o\":15,\"b\":5,\"n\":-17,\"u\":1000,\"e\":1000}");
    checkEq("dates stay text", tomlText("d = 1979-05-27\nt = 1979-05-27T07:32:00Z\n"), "{\"d\":\"1979-05-27\",\"t\":\"1979-05-27T07:32:00Z\"}");
    checkEq("quoted keys", tomlText("\"a b\" = 1\n'c' = 2\n"), "{\"a b\":1,\"c\":2}");
    for bad in ["a =", "a = 1\na = 2", "[t\nx = 1", "x = 'open", "x = [1, 2", "= 1", "x = 1 y = 2", "x = nope", "[a]\nb = 1\n[a.b]\nc = 2"] {
        check("rejects `${bad}`", strings.startsWith(tomlText(bad), "ERROR"));
    }
    checkEq("error line", tomlText("a = 1\nb = \nc = 3"), "ERROR line 2: expected a value");
}

// --- fs, os, rand additions -----------------------------------------------------------

func testStdExtras(dir: str) {
    let base = path.join(dir, "extras");
    check("mkdirs", fs.mkdirs(path.join(base, "a/b/c")));
    check("mkdirs again", fs.mkdirs(path.join(base, "a/b/c")));
    check("mkdirs is dir", fs.isDir(path.join(base, "a/b/c")));
    check("write nested", fs.write(path.join(base, "a/b/c/f.txt"), "hello"));
    check("write sibling", fs.write(path.join(base, "a/top.txt"), "x"));
    checkEqInt("walk count", len(fs.walk(base)), 5);
    check("walk lists nested", contains(fs.walk(base), path.join(base, "a/b/c/f.txt")));
    check("copy", fs.copy(path.join(base, "a/b/c/f.txt"), path.join(base, "copy.txt")));
    checkEq("copy content", fs.read(path.join(base, "copy.txt")), "hello");
    check("copy missing", !fs.copy(path.join(base, "nope"), path.join(base, "x")));
    check("removeAll", fs.removeAll(base));
    check("removeAll gone", !fs.exists(base));
    check("removeAll missing is fine", fs.removeAll(base));
    check("home and temp", strings.len(os.tempDir()) > 0);
    var id = rand.uuid();
    checkEqInt("uuid length", strings.len(id), 36);
    check("uuid shape", regex.isMatch("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$", id));
    check("uuid differs", rand.uuid() != id);
    var xs = [1, 2, 3, 4, 5, 6, 7, 8];
    rand.shuffle(xs);
    checkEqInt("shuffle keeps elements", lists.sumInt(xs), 36);
    checkEqInt("shuffle keeps length", len(xs), 8);
    check("pick", rand.pick([5], 0) == 5 && rand.pick(lists.repeat(2, 3), 0) == 2);
    var none: [int] = [];
    checkEqInt("pick empty", rand.pick(none, 9), 9);
}

// --- net, httpd, sqlite -----------------------------------------------------------

func readAllFrom(conn: int): str {
    var got = "";
    var chunk = net.read(conn, 4096);
    while !strings.isEmpty(chunk) {
        got = got + chunk;
        chunk = net.read(conn, 4096);
    }
    return got;
}

func testNet() {
    check("net available", net.isAvailable());
    let server = net.listen("127.0.0.1", 0);
    check("listen", server > 0);
    let port = net.port(server);
    check("port", port > 0 && port < 65536);
    let client = net.connect("127.0.0.1", port);
    let conn = net.accept(server, 2000);
    check("connect and accept", client > 0 && conn > 0);
    check("peer", strings.startsWith(net.peer(0), "127.0.0.1:") && strings.startsWith(net.peer(client), "127.0.0.1:"));
    checkEqInt("write", net.write(client, "hello é"), 8);
    checkEq("read", net.read(conn, 100), "hello é");
    net.write(conn, "pong");
    checkEq("reply", net.read(client, 100), "pong");
    net.setTimeout(client, 50);
    checkEq("read timeout", net.read(client, 10), "");
    checkEq("timeout reason", net.lastError(), "timeout");
    let big = strings.repeat("0123456789", 20000);
    net.write(client, big);
    net.close(client);
    var received = "";
    var chunk = net.read(conn, 65536);
    while !strings.isEmpty(chunk) {
        received = received + chunk;
        chunk = net.read(conn, 65536);
    }
    checkEqInt("large transfer", strings.byteLen(received), 200000);
    checkEq("closed reason", net.lastError(), "closed");
    net.close(conn);
    checkEqInt("accept timeout", net.accept(server, 20), -1);
    checkEq("accept timeout reason", net.lastError(), "timeout");
    net.close(server);
    checkEqInt("connect refused", net.connect("127.0.0.1", 1), -1);
    check("connect error text", !strings.isEmpty(net.lastError()));
    checkEqInt("read from nothing", strings.len(net.read(9999, 10)), 0);
    checkEq("resolve", net.resolve("127.0.0.1"), "127.0.0.1");

    let a = net.udp("127.0.0.1", 0);
    let b = net.udp("127.0.0.1", 0);
    net.setTimeout(b, 2000);
    checkEqInt("udp send", net.sendTo(a, "127.0.0.1", net.port(b), "datagram"), 8);
    checkEq("udp receive", net.recvFrom(b, 100), "datagram");
    check("udp sender", strings.endsWith(net.peer(0), ":" + strings.fromInt(net.port(a))));
    net.setTimeout(b, 30);
    checkEq("udp timeout", net.recvFrom(b, 100), "");
    net.close(a);
    net.close(b);
}

func testHttpd() {
    let srv = httpd.listen("127.0.0.1", 0);
    let port = net.port(srv);
    let c = net.connect("127.0.0.1", port);
    net.write(c, "POST /echo/a?x=1&y=2 HTTP/1.1\r\nHost: t\r\nContent-Length: 6\r\nX-Test:  yes \r\n\r\nhéllo");
    let req = httpd.next(srv);
    check("request ok", req.ok);
    checkEq("method", req.method, "POST");
    checkEq("path", req.path, "/echo/a");
    checkEq("query", req.query, "x=1&y=2");
    checkEq("header", req.headers["x-test"], "yes");
    checkEq("body with multibyte", req.body, "héllo");
    httpd.json(req, 201, "{\"ok\":true}");
    let reply = readAllFrom(c);
    net.close(c);
    check("status line", strings.startsWith(reply, "HTTP/1.1 201 Created\r\n"));
    check("content length", strings.contains(reply, "Content-Length: 11\r\n"));
    check("body", strings.endsWith(reply, "\r\n\r\n{\"ok\":true}"));

    let bad = net.connect("127.0.0.1", port);
    net.write(bad, "garbage\r\n\r\n");
    let r2 = httpd.next(srv);
    check("garbage rejected", !r2.ok);
    check("400 sent", strings.startsWith(readAllFrom(bad), "HTTP/1.1 400"));
    net.close(bad);
    check("nextWithin times out", !httpd.nextWithin(srv, 20).ok);
    checkEq("mime", httpd.mimeType("a/B.PNG"), "image/png");
    httpd.stop(srv);

    // the http client talking to the server: curl runs in the background
    if os.isWindows() {
        return;
    }
    if strings.isEmpty(os.output("command -v curl")) {
        return;
    }
    let s2 = httpd.listen("127.0.0.1", 0);
    let url = "http://127.0.0.1:" + strings.fromInt(net.port(s2)) + "/hi?name=ada";
    let out = path.join(os.tempDir(), "solis-test-" + strings.fromInt(time.nowMs()) + ".out");
    os.run("curl -s -D - -H 'X-Mine: 7' -d 'payload' -o " + out + " '" + url + "' > " + out + ".h 2>/dev/null &");
    let seen = httpd.nextWithin(s2, 5000);
    check("curl request arrived", seen.ok && seen.method == "POST" && seen.path == "/hi" && seen.body == "payload");
    checkEq("curl header", seen.headers["x-mine"], "7");
    httpd.text(seen, 200, "served");
    var waited = 0;
    while !fs.exists(out + ".h") && waited < 50 {
        time.sleepMs(100);
        waited += 1;
    }
    time.sleepMs(200);
    checkEq("curl got the body", fs.read(out), "served");
    fs.remove(out);
    fs.remove(out + ".h");
    httpd.stop(s2);

    // http.get against our own server, in the background shell
    let s3 = httpd.listen("127.0.0.1", 0);
    let u3 = "http://127.0.0.1:" + strings.fromInt(net.port(s3)) + "/ping";
    let done = path.join(os.tempDir(), "solis-test-" + strings.fromInt(time.nowMs()) + ".done");
    os.run("(curl -s -o " + done + " '" + u3 + "') &");
    let hit = httpd.nextWithin(s3, 5000);
    check("get arrived", hit.ok && hit.method == "GET" && hit.path == "/ping");
    httpd.respondWith(hit, 200, "text/plain", {"X-Served": "yes"}, "pong");
    time.sleepMs(300);
    checkEq("get body", fs.read(done), "pong");
    fs.remove(done);
    httpd.stop(s3);
}

func testSqlite() {
    if !sqlite.available() {
        return;
    }
    check("version", strings.startsWith(sqlite.version(), "3."));
    let db = sqlite.open(":memory:");
    check("open", db > 0);
    var none: [str] = [];
    checkEqInt("create", sqlite.exec(db, "create table t (id integer primary key, name text, score real)", none), 0);
    checkEqInt("insert changes", sqlite.exec(db, "insert into t (name, score) values (?, ?)", ["Ada", "9.5"]), 1);
    checkEqInt("last id", sqlite.lastInsertId(db), 1);
    checkEqInt("two statements", sqlite.exec(db, "insert into t (name) values ('Bo'); insert into t (name) values ('Cy')", none), 2);
    let rows = sqlite.query(db, "select id, name, score from t order by id", none);
    checkEqInt("row count", len(rows), 4);
    checkEq("column names", strings.join(rows[0], ","), "id,name,score");
    checkEq("cell", rows[1][1], "Ada");
    checkEq("real", rows[1][2], "9.5");
    checkEq("null is empty", rows[2][2], "");
    let recs = sqlite.records(db, "select name from t where id > ? order by id", ["1"]);
    checkEqInt("records", len(recs), 2);
    checkEq("record value", recs[1]["name"], "Cy");
    checkEq("value", sqlite.value(db, "select count(*) from t", none, "?"), "3");
    checkEq("value fallback", sqlite.value(db, "select name from t where id = 99", none, "none"), "none");
    checkEqInt("syntax error", sqlite.exec(db, "insert into nope values (1)", none), -1);
    checkEq("error text", sqlite.error(), "no such table: nope");
    checkEqInt("param mismatch", sqlite.exec(db, "select ?", none), -1);
    checkEq("injection stays data", sqlite.value(db, "select ?", ["x'; drop table t; --"], ""), "x'; drop table t; --");
    checkEq("unicode", sqlite.value(db, "select ?", ["héllo"], ""), "héllo");
    sqlite.close(db);
    checkEqInt("closed handle", sqlite.exec(db, "select 1", none), -1);

    let file = path.join(os.tempDir(), "solis-test-" + strings.fromInt(time.nowMs()) + ".db");
    let disk = sqlite.open(file);
    sqlite.run(disk, "create table k (v text)");
    sqlite.exec(disk, "insert into k values (?)", ["kept"]);
    sqlite.close(disk);
    let again = sqlite.open(file);
    checkEq("persisted", sqlite.value(again, "select v from k", none, ""), "kept");
    sqlite.close(again);
    fs.remove(file);
    checkEqInt("bad path", sqlite.open("/nonexistent-dir/x.db"), -1);
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
    testStrings();
    testPath();
    testJson();
    testProcess();
    testEncodings();
    testRegex();
    testCsv();
    testUrl();
    testDatetime();
    testLists();
    testFlags();
    testMathExtras();
    testStringsExtras();
    testLogAndHttp();
    testTextAndAnsi();
    testToml();
    testStdExtras(dir);
    testNet();
    testHttpd();
    testSqlite();

    if failures == 0 {
        print("batteries: ok");
        return;
    }
    io.errLine("batteries: ${failures} failure(s)");
    os.exit(1);
}
