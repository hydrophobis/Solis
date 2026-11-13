#!/usr/bin/env python3
"""Rewrite rules for the register-code optimizer.

    tools/optrules.py --verify        check every rule in opt/rules.txt
    tools/optrules.py --gen           write runtime/opt_rules.inc

--verify gives each rule one of two verdicts:
  proved   both sides are branch-free integer arithmetic, so each is turned into
           an affine form over the input registers and immediates (wrapping
           arithmetic modulo 2**64) and the two forms are compared. That is a
           complete decision procedure for affine expressions, so the rule holds
           for every input.
  tested   anything involving a branch is run on a grid of inputs, including the
           extremes of the 64-bit range, and the registers and branch decision are
           compared. This is evidence, not a proof.
Registers that the rule declares dead(...) are not compared.
"""
import itertools, os, re, sys

M = 1 << 64
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")

# op -> operand letters: d def reg, u use reg, i/k immediate, j jump
SPEC = {
    "LDI": "di", "MOVS": "du", "ADD_I": "duu", "SUB_I": "duu", "MUL_I": "duu", "ADDI": "dui",
    "NEG_I": "du", "ALEN_L": "du",
}
for c in ("EQ", "NE", "LT", "LE", "GT", "GE"):
    SPEC["JC_%s_I" % c] = "uuj"
    SPEC["JCI_%s" % c] = "uij"
CMP = {"EQ": lambda a, b: a == b, "NE": lambda a, b: a != b, "LT": lambda a, b: a < b,
       "LE": lambda a, b: a <= b, "GT": lambda a, b: a > b, "GE": lambda a, b: a >= b}

class Rule:
    def __init__(self, name):
        self.name = name; self.pat = []; self.cond = []; self.rep = []

def tokens(s):
    return re.findall(r"\([^)]*\)|\S+", s)

def parse_instr(text):
    t = tokens(text.strip())
    return (t[0], t[1:]) if t else None

def parse(path):
    rules, cur, stage = [], None, None
    for raw in open(path):
        line = raw.split("#")[0].rstrip()
        if not line.strip():
            continue
        s = line.strip()
        if s.startswith("rule "):
            cur = Rule(s.split()[1]); rules.append(cur); stage = "pat"; continue
        if s.startswith("when "):
            cur.cond = s[5:].split(); continue
        if s.startswith("=>"):
            body = s[2:].strip()
            cur.rep = [parse_instr(p) for p in body.split(";")] if body else []
            cur.rep = [r for r in cur.rep if r]
            continue
        if stage == "pat" and not cur.pat:
            cur.pat = [parse_instr(p) for p in s.split(";")]
    return rules

def classify(op, args):
    """name -> kind for every variable operand."""
    spec = SPEC[op]
    out = []
    for letter, a in zip(spec, args):
        if letter in "du": out.append(("reg", a))
        elif letter in "ik": out.append(("imm", a))
        else: out.append(("jump", a))
    return out

def expr_terms(e):
    """'(k1+k2)' or 'k1' or '0-k1' or '7' -> (const, {var: coef})"""
    e = e.strip("()")
    const, vs = 0, {}
    for sign, tok in re.findall(r"([+-]?)\s*([A-Za-z_]\w*|\d+)", e.replace("0-", "-0+") if False else e):
        sg = -1 if sign == "-" else 1
        if tok.isdigit(): const += sg * int(tok)
        else: vs[tok] = vs.get(tok, 0) + sg
    return const, vs

# ---- affine forms -----------------------------------------------------------
def aff_const(c): return {"1": c % M}
def aff_var(v): return {v: 1}
def aff_add(a, b):
    r = dict(a)
    for k, v in b.items(): r[k] = (r.get(k, 0) + v) % M
    return {k: v for k, v in r.items() if v}
def aff_neg(a): return {k: (-v) % M for k, v in a.items() if (-v) % M}
def aff_mul(a, b):
    ca = a.get("1", 0) if set(a) <= {"1"} else None
    cb = b.get("1", 0) if set(b) <= {"1"} else None
    if cb is not None: return {k: (v * cb) % M for k, v in a.items() if (v * cb) % M}
    if ca is not None: return {k: (v * ca) % M for k, v in b.items() if (v * ca) % M}
    raise ValueError("non-affine product")

def run_affine(instrs, regs, imms):
    for op, args in instrs:
        if op == "LDI": regs[args[0]] = imms(args[1])
        elif op == "MOVS": regs[args[0]] = regs[args[1]]
        elif op == "ADD_I": regs[args[0]] = aff_add(regs[args[1]], regs[args[2]])
        elif op == "SUB_I": regs[args[0]] = aff_add(regs[args[1]], aff_neg(regs[args[2]]))
        elif op == "MUL_I": regs[args[0]] = aff_mul(regs[args[1]], regs[args[2]])
        elif op == "NEG_I": regs[args[0]] = aff_neg(regs[args[1]])
        elif op == "ADDI": regs[args[0]] = aff_add(regs[args[1]], imms(args[2]))
        elif op == "ALEN_L": regs[args[0]] = aff_var("len_" + args[1])    # the array register is not written by the rule
        else: raise KeyError(op)

def imm_affine(tok):
    const, vs = expr_terms(tok)
    r = aff_const(const)
    for v, c in vs.items(): r = aff_add(r, {"imm:" + v: c % M})
    return {k: x for k, x in r.items() if x}

def regs_of(rule):
    names = []
    for op, args in rule.pat + rule.rep:
        for kind, a in classify(op, args):
            if kind == "reg" and a not in names: names.append(a)
    return names

def dead_set(rule):
    return {c[5:-1] for c in rule.cond if c.startswith("dead(")}

def prove(rule):
    names = regs_of(rule)
    imm = lambda tok: imm_affine(tok)
    a = {n: aff_var(n) for n in names}
    b = {n: aff_var(n) for n in names}
    run_affine(rule.pat, a, imm)
    run_affine(rule.rep, b, imm)
    dead = dead_set(rule)
    bad = [n for n in names if n not in dead and a[n] != b[n]]
    return not bad, bad

# ---- grid testing -------------------------------------------------------------
def wrap(x):
    x %= M
    return x - M if x >= M // 2 else x

def run_concrete(instrs, regs, imm_vals):
    taken = None
    for op, args in instrs:
        def reg(i): return regs[args[i]]
        def imm(tok):
            const, vs = expr_terms(tok)
            return const + sum(c * imm_vals[v] for v, c in vs.items())
        if op == "LDI": regs[args[0]] = wrap(imm(args[1]))
        elif op == "MOVS": regs[args[0]] = reg(1)
        elif op == "ADD_I": regs[args[0]] = wrap(reg(1) + reg(2))
        elif op == "SUB_I": regs[args[0]] = wrap(reg(1) - reg(2))
        elif op == "MUL_I": regs[args[0]] = wrap(reg(1) * reg(2))
        elif op == "NEG_I": regs[args[0]] = wrap(-reg(1))
        elif op == "ADDI": regs[args[0]] = wrap(reg(1) + imm(args[2]))
        elif op.startswith("JC_") and op.endswith("_I"): taken = CMP[op[3:5]](reg(0), reg(1))
        elif op.startswith("JCI_"): taken = CMP[op[4:6]](reg(0), imm(args[1]))
        else: raise KeyError(op)
    return taken

def test(rule):
    names = regs_of(rule)
    immvars = sorted({a for op, args in rule.pat + rule.rep for kind, a in classify(op, args) if kind == "imm" and not a.lstrip("-").isdigit() and re.match(r"^k\w*$", a)})
    dead = dead_set(rule)
    ne = [(c[3:-1].split(",")) for c in rule.cond if c.startswith("ne(")]
    eq = [(c[3:-1].split(",")) for c in rule.cond if c.startswith("eq(")]
    vals = [-3, -1, 0, 1, 2, 5, 100, -100, 2**62, -2**62, 2**63 - 1, -2**63]
    imms = [-3, -1, 0, 1, 2, 7, 300, -300, 32767, -32768]
    n = 0
    for rv in itertools.product(vals, repeat=len(names)):
        env0 = dict(zip(names, rv))
        # registers that alias must hold the same value, the rest are independent
        for iv in itertools.product(imms, repeat=len(immvars)):
            ivals = dict(zip(immvars, iv))
            ok = True
            for c in rule.cond:
                if c.startswith("fits16("):
                    const, vs = expr_terms(c[7:-1])
                    x = const + sum(k * ivals[v] for v, k in vs.items())
                    if not -32768 <= x <= 32767: ok = False
                if c.startswith("nonneg("):
                    const, vs = expr_terms(c[7:-1])
                    if const + sum(k * ivals[v] for v, k in vs.items()) < 0: ok = False
            if not ok: continue
            a, b = dict(env0), dict(env0)
            for x, y in ne:
                if x == y: ok = False
            for x, y in eq:
                b[y] = a[y] = a[x]
            if not ok: continue
            ta = run_concrete(rule.pat, a, ivals)
            tb = run_concrete(rule.rep, b, ivals)
            n += 1
            if ta != tb: return False, "branch differs at %s %s" % (env0, ivals), n
            for r in names:
                if r in dead: continue
                if a[r] != b[r]: return False, "reg %s differs at %s %s: %s vs %s" % (r, env0, ivals, a[r], b[r]), n
    return True, "", n

def verify(rules):
    bad = 0
    print("%-22s %-8s %s" % ("rule", "verdict", "detail"))
    for r in rules:
        branchy = any(op.startswith("J") for op, _ in r.pat + r.rep)
        nonaffine = any(op == "MUL_I" for op, _ in r.pat + r.rep)
        if not branchy:
            try:
                ok, which = prove(r)
                print("%-22s %-8s %s" % (r.name, "proved" if ok else "FAILED", "" if ok else "differs in " + ",".join(which)))
                if not ok: bad += 1
                continue
            except ValueError:
                pass
        ok, why, n = test(r)
        print("%-22s %-8s %s" % (r.name, "tested" if ok else "FAILED", ("%d cases" % n) if ok else why))
        if not ok: bad += 1
    return bad

# ---- C generation --------------------------------------------------------------
def gen(rules):
    out = ["// Generated by tools/optrules.py from opt/rules.txt. Do not edit.", ""]
    out.append("static const orule opt_rules[] = {")
    for r in rules:
        vmap = {"reg": {}, "imm": {}, "jump": {}}
        def vid(kind, name):
            return vmap[kind].setdefault(name, len(vmap[kind]))
        # patterns
        pat_c = []
        for op, args in r.pat:
            spec = SPEC[op]
            a_c = []
            for letter, a in zip(spec, args):
                if letter in "du": a_c.append("{ OK_REG, %d, {0} }" % vid("reg", a))
                elif letter in "ik":
                    if re.match(r"^-?\d+$", a): a_c.append("{ OK_LIT, %d, {0} }" % int(a))
                    else: a_c.append("{ OK_IMM, %d, {0} }" % vid("imm", a))
                else: a_c.append("{ OK_JUMP, %d, {0} }" % vid("jump", a))
            pat_c.append("{ R_%s, %d, { %s } }" % (op, len(args), ", ".join(a_c)))
        rep_c = []
        for op, args in r.rep:
            spec = SPEC[op]
            a_c = []
            for letter, a in zip(spec, args):
                if letter in "du": a_c.append("{ OK_REG, %d, {0} }" % vid("reg", a))
                elif letter in "ik":
                    const, vs = expr_terms(a)
                    ks = [0, 0, 0, 0]
                    for v, c in vs.items(): ks[vid("imm", v)] = c
                    a_c.append("{ OK_EXPR, %d, { %s } }" % (const, ", ".join(map(str, ks))))
                else: a_c.append("{ OK_JUMP, %d, {0} }" % vid("jump", a))
            rep_c.append("{ R_%s, %d, { %s } }" % (op, len(args), ", ".join(a_c)))
        cond_c = []
        for c in r.cond:
            m = re.match(r"(\w+)\((.*)\)", c)
            kind, body = m.group(1), m.group(2)
            if kind == "dead": cond_c.append("{ OC_DEAD, %d, 0, 0, {0} }" % vid("reg", body))
            elif kind in ("ne", "eq"):
                x, y = body.split(",")
                cond_c.append("{ %s, %d, %d, 0, {0} }" % ("OC_NE" if kind == "ne" else "OC_EQ", vid("reg", x), vid("reg", y)))
            elif kind in ("fits16", "nonneg"):
                const, vs = expr_terms(body)
                ks = [0, 0, 0, 0]
                for v, cf in vs.items(): ks[vid("imm", v)] = cf
                cond_c.append("{ %s, 0, 0, %d, { %s } }" % ("OC_FITS16" if kind == "fits16" else "OC_NONNEG", const, ", ".join(map(str, ks))))
        out.append("    { \"%s\", %d, %d, %d, { %s }, { %s }, { %s } }," % (
            r.name, len(pat_c), len(rep_c), len(cond_c), ", ".join(pat_c), ", ".join(rep_c), ", ".join(cond_c)))
    out.append("};")
    out.append("#define OPT_NRULES (sizeof opt_rules / sizeof opt_rules[0])")
    return "\n".join(out) + "\n"

if __name__ == "__main__":
    rules = parse(os.path.join(ROOT, "opt", "rules.txt"))
    if "--verify" in sys.argv:
        bad = verify(rules)
        print("\n%d rule(s), %d failed" % (len(rules), bad))
        sys.exit(1 if bad else 0)
    if "--check" in sys.argv:
        # every rule verifies, and runtime/opt_rules.inc is what --gen would write now
        if verify(rules):
            sys.exit(1)
        want = gen(rules)
        have = open(os.path.join(ROOT, "runtime", "opt_rules.inc")).read()
        if want != have:
            print("runtime/opt_rules.inc is out of date: run tools/optrules.py --gen")
            sys.exit(1)
        print("rules verified, generated table up to date")
        sys.exit(0)
    if "--gen" in sys.argv:
        if verify(rules):
            print("refusing to generate: a rule failed"); sys.exit(1)
        open(os.path.join(ROOT, "runtime", "opt_rules.inc"), "w").write(gen(rules))
        print("wrote runtime/opt_rules.inc")
        sys.exit(0)
    print(__doc__)
