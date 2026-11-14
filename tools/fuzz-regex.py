#!/usr/bin/env python3
"""Checks the regex module against Python's re on random patterns and texts.

    python tools/fuzz-regex.py [count] [seed]

Patterns stay inside the syntax both engines share, so any difference in
isMatch, find, findAll, replace or split is a bug in one of them.
"""
import os, random, re, signal, subprocess, sys, tempfile, time

count = int(sys.argv[1]) if len(sys.argv) > 1 else 300
seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
rng = random.Random(seed)
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
solis = os.path.join(root, "solis")

ATOMS = ["a", "b", "c", " ", "1", ".", r"\d", r"\w", r"\s", r"\D", r"\W", "[abc]", "[^a]", "[a-c1]", r"[\d ]", "é"]
QUANT = ["*", "+", "?", "{2}", "{1,2}", "{0,2}", "{2,}", "*?", "+?", "??", "{1,3}?"]

def gen(depth=0):
    parts = []
    for _ in range(rng.randint(1, 4)):
        r = rng.random()
        if depth < 2 and r < 0.2:
            inner = "|".join(gen(depth + 1) for _ in range(rng.randint(1, 3)))
            atom = rng.choice(["(%s)", "(?:%s)"]) % inner
        elif r < 0.27:
            atom = rng.choice([r"\b", "^", "$"])
            parts.append(atom)
            continue
        else:
            atom = rng.choice(ATOMS)
        if rng.random() < 0.35:
            atom += rng.choice(QUANT)
        parts.append(atom)
    return "".join(parts)

def gen_text():
    # no trailing newline: Python lets `$` match before one, this engine does not
    return "".join(rng.choice("abc 1_-\né") for _ in range(rng.randint(0, 12))).rstrip("\n")

def s_lit(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("$", "\\$") + '"'

def py_repr(items):
    return "[" + ",".join(items) + "]"

def cheap(rx, text):
    """Patterns that make Python backtrack for long are skipped; they would
    also exhaust this engine's step budget, which is an error by design.
    Python's matcher cannot be interrupted from inside, so the probe runs in a
    forked child that is killed if it takes too long."""
    pid = os.fork()
    if pid == 0:
        try:
            rx.search(text); list(rx.finditer(text)); rx.sub("", text); rx.split(text)
        finally:
            os._exit(0)
    for _ in range(30):
        done, _status = os.waitpid(pid, os.WNOHANG)
        if done:
            return True
        time.sleep(0.005)
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
    return False

cases = []
while len(cases) < count:
    pat = gen()
    try:
        rx = re.compile(pat)
    except re.error:
        continue
    text = gen_text()
    if cheap(rx, text):
        cases.append((pat, rx, text))

lines = ["import regex;", "import strings;", 'func p(s: str) { print(strings.replace(s, "\\n", "~")); }', "func main() {"]
expected = []
for i, (pat, rx, text) in enumerate(cases):
    p, t = s_lit(pat), s_lit(text)
    lines.append('    p("M" + strings.fromBool(regex.isMatch(%s, %s)));' % (p, t))
    lines.append('    p("F[" + regex.find(%s, %s) + "]");' % (p, t))
    lines.append('    p("A[" + strings.join(regex.findAll(%s, %s), "|") + "]");' % (p, t))
    lines.append('    p("R[" + regex.replace(%s, %s, "<$0>") + "]");' % (p, t))
    m = rx.search(text)
    expected.append("M" + ("true" if m else "false"))
    expected.append("F[" + (m.group(0) if m else "") + "]")
    expected.append("A[" + "|".join(x.group(0) for x in rx.finditer(text)) + "]")
    expected.append("R[" + rx.sub(lambda x: "<" + x.group(0) + ">", text) + "]")
    if "(" not in pat.replace("(?:", ""):
        lines.append('    p("S[" + strings.join(regex.split(%s, %s), "|") + "]");' % (p, t))
        expected.append("S[" + "|".join(rx.split(text)) + "]")
lines.append("}")

with tempfile.NamedTemporaryFile("w", suffix=".sl", delete=False, encoding="utf-8") as f:
    f.write("\n".join(lines))
    path = f.name
out = subprocess.run([solis, path], capture_output=True, text=True, timeout=120)
os.unlink(path)
if out.returncode != 0:
    print(out.stderr[:2000])
    sys.exit(1)
got = out.stdout.split("\n")
expected[:] = [e.replace("\n", "~") for e in expected]
if got and got[-1] == "":
    got.pop()
bad = 0
k = 0
for pat, rx, text in cases:
    n = 5 if "(" not in pat.replace("(?:", "") else 4
    chunk_got, chunk_exp = got[k:k + n], expected[k:k + n]
    k += n
    if chunk_got != chunk_exp:
        bad += 1
        if bad <= 5:
            print("pattern %r text %r\n  got      %r\n  expected %r" % (pat, text, chunk_got, chunk_exp))
print("%d/%d cases agree" % (len(cases) - bad, len(cases)))
sys.exit(1 if bad else 0)
