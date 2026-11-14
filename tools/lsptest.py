#!/usr/bin/env python3
"""Drives `sl lsp` over stdio and checks each supported request."""
import json, os, subprocess, sys, tempfile

SL = os.path.join(os.path.dirname(__file__), "..", "tooling", "sl")
proc = subprocess.Popen([SL, "lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
next_id = 0
pending = []
failures = 0

def send(obj):
    body = json.dumps(obj).encode()
    proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    proc.stdin.flush()

def read():
    n = None
    while True:
        line = proc.stdout.readline()
        if not line:
            raise SystemExit("server closed")
        line = line.strip()
        if not line:
            break
        if line.lower().startswith(b"content-length:"):
            n = int(line.split(b":")[1])
    return json.loads(proc.stdout.read(n))

def request(method, params):
    global next_id
    next_id += 1
    send({"jsonrpc": "2.0", "id": next_id, "method": method, "params": params})
    while True:
        m = read()
        if m.get("id") == next_id:
            return m
        pending.append(m)

def notify(method, params):
    send({"jsonrpc": "2.0", "method": method, "params": params})

def diagnostics_for(uri):
    for i, m in enumerate(pending):
        if m.get("method") == "textDocument/publishDiagnostics" and m["params"]["uri"] == uri:
            pending.pop(i)
            return m["params"]["diagnostics"]
    while True:
        m = read()
        if m.get("method") == "textDocument/publishDiagnostics" and m["params"]["uri"] == uri:
            return m["params"]["diagnostics"]
        pending.append(m)

def check(cond, what):
    global failures
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures += 1

good = """struct Point {
    x: int;
    y: int;
}

func add(a: int, b: int): int {
    return a + b;
}

func main() {
    let p = Point { x: 1, y: 2 };
    let total = add(p.x, p.y);
    print("é total ${total}");
}
"""

tmp = tempfile.mkdtemp()
path = os.path.join(tmp, "main.sl")
uri = "file://" + path
open(path, "w").write(good)

r = request("initialize", {"processId": None, "rootUri": "file://" + tmp, "capabilities": {}})
caps = r["result"]["capabilities"]
check(caps["hoverProvider"] and caps["definitionProvider"] and caps["referencesProvider"], "capabilities advertised")
notify("initialized", {})

notify("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "solis", "version": 1, "text": good}})
check(diagnostics_for(uri) == [], "clean file has no diagnostics")

def pos(text, needle, nth=0, delta=0):
    idx = -1
    for _ in range(nth + 1):
        idx = text.index(needle, idx + 1)
    idx += delta
    line = text.count("\n", 0, idx)
    col = idx - (text.rfind("\n", 0, idx) + 1)
    return {"line": line, "character": col}

tdoc = {"uri": uri}

r = request("textDocument/hover", {"textDocument": tdoc, "position": pos(good, "add(p.x", 0, 1)})
check(r["result"] and "add" in r["result"]["contents"]["value"] and "int" in r["result"]["contents"]["value"], "hover on function call")

r = request("textDocument/definition", {"textDocument": tdoc, "position": pos(good, "add(p.x", 0, 1)})
check(r["result"] and r["result"]["range"]["start"]["line"] == 5, "definition of add points at its declaration")

r = request("textDocument/references", {"textDocument": tdoc, "position": pos(good, "func add", 0, 6), "context": {"includeDeclaration": True}})
check(r["result"] is not None and len(r["result"]) == 2, "references to add: declaration and one call")
r = request("textDocument/references", {"textDocument": tdoc, "position": pos(good, "func add", 0, 6), "context": {"includeDeclaration": False}})
check(r["result"] is not None and len(r["result"]) == 1, "references exclude the declaration on request")

r = request("textDocument/documentSymbol", {"textDocument": tdoc})
names = [s["name"] for s in r["result"]]
check("Point" in names and "add" in names and "main" in names, "document symbols")

# completion after a '.'
typing = good.replace("let total = add(p.x, p.y);", "let total = p.")
notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": typing}]})
diagnostics_for(uri)
r = request("textDocument/completion", {"textDocument": tdoc, "position": pos(typing, "p.\n", 0, 2)})
labels = [i["label"] for i in r["result"]["items"]]
check("x" in labels and "y" in labels, "member completion after '.'")

# an error, positioned after a multibyte character on an earlier line
bad = good.replace("add(p.x, p.y)", "add(p.x, \"no\")")
notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 3}, "contentChanges": [{"text": bad}]})
d = diagnostics_for(uri)
check(len(d) >= 1 and d[0]["range"]["start"]["line"] == 11, "type error reported on the right line")

# queries keep working while the buffer does not parse: the last good analysis
# answers, translated across the edit (two lines inserted in the middle)
broken = good.replace("func main() {", "// c\nfunc oops( {\nfunc main() {")
notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 4}, "contentChanges": [{"text": good}]})
diagnostics_for(uri)
notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 5}, "contentChanges": [{"text": broken}]})
d = diagnostics_for(uri)
check(len(d) >= 1, "syntax error reported")
r = request("textDocument/hover", {"textDocument": tdoc, "position": pos(broken, "add(p.x", 0, 1)})
check(r["result"] and "add" in r["result"]["contents"]["value"], "hover answered from the last good analysis")
r = request("textDocument/definition", {"textDocument": tdoc, "position": pos(broken, "p.x", 0, 0)})
check(r["result"] and r["result"]["range"]["start"]["line"] == 12, "definition translated across the edit")
r = request("textDocument/references", {"textDocument": tdoc, "position": pos(broken, "func add", 0, 6), "context": {"includeDeclaration": True}})
check(r["result"] is not None and len(r["result"]) == 2, "references while broken")
r = request("textDocument/hover", {"textDocument": tdoc, "position": pos(broken, "oops", 0, 1)})
check(r["result"] is None, "no answer inside the edited span")
notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 6}, "contentChanges": [{"text": good}]})
diagnostics_for(uri)

# an import resolves through an open buffer
lib = os.path.join(tmp, "lib.sl")
open(lib, "w").write("func one(): int { return 1; }\n")
src = "import lib;\n\nfunc main() {\n    let x = lib.two();\n}\n"
notify("textDocument/didOpen", {"textDocument": {"uri": "file://" + lib, "languageId": "solis", "version": 1, "text": "func two(): int { return 2; }\n"}})
diagnostics_for("file://" + lib)
main2 = os.path.join(tmp, "m2.sl")
open(main2, "w").write(src)
notify("textDocument/didOpen", {"textDocument": {"uri": "file://" + main2, "languageId": "solis", "version": 1, "text": src}})
d = diagnostics_for("file://" + main2)
check(d == [], "import sees unsaved buffer of the imported file")

r = request("textDocument/nothing", {})
check(r.get("error", {}).get("code") == -32601, "unknown request answered with method-not-found")

r = request("shutdown", None)
notify("exit", None)
check(proc.wait(timeout=5) == 0, "clean exit after shutdown")
sys.exit(1 if failures else 0)
