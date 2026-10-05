#!/usr/bin/env bash
#
# The stdlib reaches the compiler two ways: #embed reads std/*.sl, and
# std_fallback.h is a committed copy for compilers without it, which can go
# stale silently, since a toolchain with #embed never compiles it.
#
# Builds the toolchain both ways and requires identical bytecode.
# Fix a mismatch with: python compiler/embed_std.py
#
set -uo pipefail
cd "$(dirname "$0")/.."

CC=${CC:-cc}
command -v "$CC" >/dev/null 2>&1 || CC=gcc

OUT=${TMPDIR:-/tmp}/solis-fallback
mkdir -p "$OUT"

# C99 on purpose: the fallback exists for old compilers, so it is checked the
# way they would see it.
STD=-std=c99

SRC="compiler/*.c runtime/*.c cli/solis.c"
INC="-Icompiler -Iruntime"

if ! "$CC" $STD -O1 $INC -o "$OUT/embed" $SRC -lm 2>"$OUT/embed.log"; then
    echo "the #embed build failed:" >&2
    cat "$OUT/embed.log" >&2
    exit 1
fi
if ! "$CC" $STD -O1 -DSOLIS_NO_EMBED $INC -o "$OUT/fallback" $SRC -lm 2>"$OUT/fb.log"; then
    echo "the fallback build failed:" >&2
    cat "$OUT/fb.log" >&2
    exit 1
fi

# A program that imports both stdlib modules, so the comparison covers them.
probe=examples/stdlib.sl

"$OUT/embed" -o "$OUT/embed.slb" "$probe" >/dev/null || exit 1
"$OUT/fallback" -o "$OUT/fallback.slb" "$probe" >/dev/null || exit 1

if cmp -s "$OUT/embed.slb" "$OUT/fallback.slb"; then
    echo "std_fallback.h matches std/*.sl"
    exit 0
fi

echo "std_fallback.h is stale - it disagrees with std/*.sl" >&2
echo "regenerate it with: python compiler/embed_std.py" >&2
exit 1
