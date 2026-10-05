#!/usr/bin/env python3
"""Recover std/*.sl from std_fallback.h.

The inverse of embed_std.py. The fallback holds each stdlib module verbatim as
one C string literal per source line, so the original can be rebuilt from it
exactly. Useful if the sources are ever lost while the generated copy survives.

    python compiler/unembed_std.py
"""

import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "std_fallback.h")
OUT = os.path.join(HERE, "..", "std")


def unescape(s):
    out = []
    i = 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            c = s[i + 1]
            out.append({"n": "\n", "t": "\t", "\\": "\\", '"': '"'}.get(c, c))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def main():
    text = io.open(SRC, encoding="utf-8").read()
    blocks = re.findall(
        r"static const char src_(\w+)\[\] =\n(.*?);\n", text, re.S)
    if not blocks:
        sys.stderr.write("no embedded modules found in std_src.c\n")
        return 1

    if not os.path.isdir(OUT):
        os.makedirs(OUT)

    for name, body in blocks:
        parts = re.findall(r'^\s*"(.*)"$', body, re.M)
        source = "".join(unescape(p) for p in parts)
        # embed_std.py splits on "\n", so the final split produces a trailing
        # empty piece that becomes one extra newline. Drop it.
        if source.endswith("\n\n"):
            source = source[:-1]
        path = os.path.join(OUT, "%s.sl" % name)
        io.open(path, "w", encoding="utf-8", newline="\n").write(source)
        print("  std/%s.sl  %d bytes" % (name, len(source)))

    print("%d modules recovered" % len(blocks))
    return 0


if __name__ == "__main__":
    sys.exit(main())
