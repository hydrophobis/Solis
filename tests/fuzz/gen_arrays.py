#!/usr/bin/env python3
"""Random programs built around array loops, for checking bounds-check removal.

Loops are guarded by len(), by a stored length, or by a constant, and their bodies
read and write elements at indices of different shapes while sometimes changing the
array's length directly, through an alias or through a call.
"""
import random, sys
R = random.Random(int(sys.argv[1]))
def idx(i):
    return R.choice([i, i, i, "%s + 1" % i, "%s - 1" % i, "len(a) - 1 - %s" % i, "%s * 2" % i, "0", "len(a)"])
def stmt(i, depth):
    r = R.random()
    ind = "    " * (depth + 2)
    if r < 0.28: return ind + "s += a[%s];" % idx(i)
    if r < 0.42: return ind + "a[%s] = s %% 97;" % idx(i)
    if r < 0.50: return ind + "s += b[%s %% len(b)];" % i
    if r < 0.56: return ind + "push(a, s %% 13);"
    if r < 0.60: return ind + "if len(a) > 1 { pop(a); }"
    if r < 0.64: return ind + "grow(a);"
    if r < 0.68: return ind + "shrink(alias);"
    if r < 0.71: return ind + "a = b;"
    if r < 0.74: return ind + "%s += %d;" % (i, R.choice([1, 1, 2, 3]))
    if r < 0.80: return ind + "if %s < len(a) { s += a[%s]; }" % (i, i)
    if r < 0.85: return ind + "if %s >= 0 && %s < len(a) { a[%s] = 1; }" % (i, i, i)
    return ind + "s += %s;" % i
def loop(depth):
    i = "i%d" % depth
    start = R.choice([0, 0, 0, 1, 2, -1])
    guard = R.choice(["%s < len(a)", "%s < len(a)", "%s < n", "%s < len(b)", "%s < 6", "%s <= len(a) - 1"]) % i
    body = [stmt(i, depth) for _ in range(R.randint(1, 4))]
    if depth < 1 and R.random() < 0.4:
        body.append(loop(depth + 1))
    # always advance the counter at the end so every program terminates
    step = "        " + "    " * depth + "%s += 1;\n" % i
    ind = "    " * (depth + 1)
    return "%svar %s = %d;\n%swhile %s {\n%s\n%s%s%s}" % (ind, i, start, ind, guard, "\n".join(body), step, ind, "")
prog = """func grow(x: [int]) { push(x, 7); }
func shrink(x: [int]) { if len(x) > 1 { pop(x); } }
func main() {
    var a = [%s];
    var b = [3, 1, 4, 1, 5];
    let alias = a;
    let n = len(a);
    var s = 0;
%s
    print("${s} ${len(a)} ${a}");
}
""" % (", ".join(str(R.randint(0, 9)) for _ in range(R.randint(1, 7))), "\n".join(loop(0) for _ in range(R.randint(1, 3))))
print(prog)
