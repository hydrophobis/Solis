#!/usr/bin/env bash
#
# Conformance. Two checks per example:
#
#   1. The C compiler's bytecode matches tests/golden/*.slb, byte for byte.
#      A regression floor. An intended codegen change shows up as a diff
#      you have to bless.
#
#   2. The C runtime's output matches tests/expected/*.txt, no leaks, and
#      interpreting the source has to match running the compiled image too.
#
# Building is Bloomery's job: run `bl conform`, or build first and call this
# directly. Blessing the golden files is tools/bless.sh.
#
#   tools/conform.sh            check every example
#   tools/conform.sh hello      check one
#
set -uo pipefail
cd "$(dirname "$0")/.."

. tools/examples.sh

OUT=${TMPDIR:-/tmp}/solis-conform
mkdir -p "$OUT"

SOLIS=./solis
[ -x "$SOLIS" ] || SOLIS=./solis.exe
[ -x "$SOLIS" ] || { echo "missing ./solis - run 'bl' first" >&2; exit 2; }

select_examples "$@" || exit 2

pass=0
fail=0

for f in "${EXAMPLES[@]}"; do
    name=$(basename "$f" .sl)
    golden=tests/golden/$name.slb
    expected=tests/expected/$name.txt

    if [ ! -f "$golden" ] || [ ! -f "$expected" ]; then
        echo "FAIL $name: nothing blessed for it yet - run tools/bless.sh"
        fail=$((fail + 1))
        continue
    fi

    # --- 1. codegen against the golden bytecode -------------------------
    if ! "$SOLIS" -o "$OUT/$name.slb" "$f" >/dev/null 2>"$OUT/$name.cerr"; then
        echo "FAIL $name: does not compile"
        sed 's/^/      /' "$OUT/$name.cerr" | head -12
        fail=$((fail + 1))
        continue
    fi
    if ! cmp -s "$golden" "$OUT/$name.slb"; then
        echo "FAIL $name: bytecode differs from tests/golden/$name.slb"
        echo "      $(wc -c < "$golden" | tr -d ' ') bytes blessed, $(wc -c < "$OUT/$name.slb" | tr -d ' ') now"
        cmp -l "$golden" "$OUT/$name.slb" 2>/dev/null | head -5 | sed 's/^/      byte /'
        echo "      if this change is intended: tools/bless.sh $name"
        fail=$((fail + 1))
        continue
    fi

    # --- 2. the C runtime against the expected output -------------------
    "$SOLIS" --check-leaks "$OUT/$name.slb" 2>"$OUT/$name.leak" | normalise > "$OUT/$name.c"
    leaks=$?
    if ! diff -q --strip-trailing-cr "$expected" "$OUT/$name.c" >/dev/null; then
        echo "FAIL $name: the C runtime's output changed"
        diff --strip-trailing-cr "$expected" "$OUT/$name.c" | sed 's/^/      /' | head -20
        echo "      if this change is intended: tools/bless.sh $name"
        fail=$((fail + 1))
        continue
    fi
    if [ "$leaks" -ne 0 ]; then
        echo "FAIL $name: $(cat "$OUT/$name.leak")"
        fail=$((fail + 1))
        continue
    fi

    # --- 2b. interpreting the source, which is the default way to run ---
    "$SOLIS" "$f" 2>&1 | normalise > "$OUT/$name.interp"
    if ! diff -q --strip-trailing-cr "$expected" "$OUT/$name.interp" >/dev/null; then
        echo "FAIL $name: interpreting differs from running the image"
        diff --strip-trailing-cr "$expected" "$OUT/$name.interp" | sed 's/^/      /' | head -20
        fail=$((fail + 1))
        continue
    fi

    bytes=$(wc -c < "$golden" | tr -d ' ')
    echo "ok   $name  ${bytes} bytes match golden, output matches, no leaks"
    pass=$((pass + 1))
done

echo
if [ "$fail" -eq 0 ]; then
    echo "$pass/$pass ok"
else
    echo "$pass passed, $fail failed"
fi
exit $((fail > 0))
