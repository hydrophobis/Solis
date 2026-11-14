#!/usr/bin/env python3
"""Compares the toml module with Python's tomllib.

    python tools/toml-check.py [file.toml ...]

With no arguments it checks a built-in set of documents and the repository's
own .toml files. Each is parsed by both and the results compared as JSON.
"""
import json, math, os, subprocess, sys, tempfile, tomllib, datetime

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
solis = os.path.join(root, "solis")

DOCS = [
    'a = 1\nb = "two"\nc = 3.5\nd = true\n',
    '# comment\n[server]\nhost = "localhost" # trailing\nport = 8_080\n\n[server.tls]\nenabled = false\n',
    '[[items]]\nname = "a"\n[[items]]\nname = "b"\n[items.extra]\nx = 1\n',
    'list = [1, 2,\n  3, # three\n]\nnested = [[1, 2], ["a"]]\nempty = []\n',
    'point = { x = 1, y = 2 }\nname.first = "Ada"\nname.last = \'Lovelace\'\n',
    'text = """\nline one\nline two \\\n   continued"""\nlit = \'\'\'raw \\n text\'\'\'\n',
    'esc = "tab\\there \\"quoted\\" \\u00e9 \\U0001F600"\n',
    'hex = 0xFF\noct = 0o17\nbin = 0b101\nneg = -17\npos = +5\nf = 6.626e-34\n',
    'when = 1979-05-27\nat = 1979-05-27T07:32:00Z\n"quoted key" = 1\n\'lit key\' = 2\n',
    '[a.b.c]\nd = 1\n[a]\ne = 2\n',
    'x = 1\n\n\n[t]\n  indented = "ok"\n',
]
for name in ("solis.toml", "bloomery.toml"):
    path = os.path.join(root, name)
    if os.path.exists(path):
        DOCS.append(open(path, encoding="utf-8").read())
for path in sys.argv[1:]:
    DOCS.append(open(path, encoding="utf-8").read())

def plain(v):
    if isinstance(v, dict):
        return {k: plain(x) for k, x in v.items()}
    if isinstance(v, list):
        return [plain(x) for x in v]
    if isinstance(v, (datetime.date, datetime.time, datetime.datetime)):
        return "DATE"
    if isinstance(v, float) and (math.isinf(v) or math.isnan(v)):
        return str(v)
    return v

def norm(v):
    """Dates by shape only; ints and floats compare by value."""
    if isinstance(v, dict):
        return {k: norm(x) for k, x in v.items()}
    if isinstance(v, list):
        return [norm(x) for x in v]
    if isinstance(v, str) and len(v) >= 8 and v[4:5] == "-" and v[:4].isdigit():
        return "DATE"
    if isinstance(v, str) and len(v) >= 8 and v[2:3] == ":" and v[:2].isdigit():
        return "DATE"
    return v

bad = 0
for n, doc in enumerate(DOCS):
    try:
        expected = norm(plain(tomllib.loads(doc)))
    except tomllib.TOMLDecodeError as e:
        print("doc %d: python rejects it (%s); skipped" % (n, e))
        continue
    prog = 'import toml;\nimport json;\nimport fs;\nimport os;\nfunc main() {\n    switch toml.parse(fs.read(os.arg(0, ""))) {\n        case Success(v): print(json.stringify(v));\n        case Failure(m): print("ERROR " + m);\n    }\n}\n'
    with tempfile.TemporaryDirectory() as d:
        open(os.path.join(d, "p.sl"), "w").write(prog)
        open(os.path.join(d, "in.toml"), "w", encoding="utf-8").write(doc)
        out = subprocess.run([solis, os.path.join(d, "p.sl"), os.path.join(d, "in.toml")], capture_output=True, text=True, timeout=60)
    text = out.stdout.strip()
    if out.returncode != 0 or text.startswith("ERROR"):
        bad += 1
        print("doc %d: solis failed: %s %s\n%s" % (n, text, out.stderr.strip()[:300], doc[:200]))
        continue
    got = norm(plain(json.loads(text.replace("Infinity", '"inf"'))))
    if got != expected:
        bad += 1
        print("doc %d differs\n  got      %r\n  expected %r" % (n, got, expected))
print("%d documents, %d differ" % (len(DOCS), bad))
sys.exit(1 if bad else 0)
