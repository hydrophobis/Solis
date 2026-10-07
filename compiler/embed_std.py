#!/usr/bin/env python3
"""Generate the no-#embed fallback copy of the stdlib source.

std_src.c reads std/*.sl with #embed where the compiler supports it, which
needs no help from anything. This exists for compilers that don't: it writes
std_fallback.h, a committed C99 copy of the same data.

    python compiler/embed_std.py

Run it after editing the stdlib if you care about pre-C23 compilers. On a
toolchain with #embed the fallback is never compiled, so a stale copy will not
show up in a local build - tools/check-fallback.sh checks it instead.
"""

import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "std")
OUT = os.path.join(HERE, "std_fallback.h")

MODULES = ["math", "strings", "io", "fs", "os", "time", "rand", "option", "result", "test", "map"]


def c_escape(text):
    """One C string literal per source line, so the result stays readable and
    diffable rather than being one enormous line."""
    out = []
    for line in text.split("\n"):
        body = (
            line.replace("\\", "\\\\")
            .replace('"', '\\"')
            .replace("\t", "\\t")
        )
        out.append('    "%s\\n"' % body)
    return "\n".join(out)


def main():
    parts = [
        "// Fallback copy of the stdlib source. GENERATED, do not edit.",
        "//",
        "// Regenerate with:  python compiler/embed_std.py",
        "//",
        "// Included by std_src.c only when the compiler has no #embed.",
        "",
    ]

    for name in MODULES:
        path = os.path.join(SRC, "%s.sl" % name)
        if not os.path.exists(path):
            sys.stderr.write("missing %s\n" % path)
            return 1
        text = io.open(path, encoding="utf-8").read()
        parts.append("static const char src_%s[] =" % name)
        parts.append(c_escape(text) + ";")
        parts.append("")

    io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(parts))
    total = sum(
        len(io.open(os.path.join(SRC, "%s.sl" % n), encoding="utf-8").read())
        for n in MODULES
    )
    print("std_fallback.h: %d modules, %d bytes of Solis" % (len(MODULES), total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
