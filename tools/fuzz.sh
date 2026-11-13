#!/usr/bin/env bash
#
# Differential fuzzing: random programs from tests/fuzz/gen.py, run through the
# built ./solis and through a reference interpreter, outputs compared. The
# reference should be a build of an earlier or independent VM; that is how the
# register VM was checked against the stack VM it replaced. Also checks that
# no run leaks.
#
#   tools/fuzz.sh <reference-solis> [count] [first-seed]
#
set -uo pipefail
cd "$(dirname "$0")/.."

REF=${1:?usage: tools/fuzz.sh <reference-solis> [count] [first-seed]}
COUNT=${2:-500}
FIRST=${3:-1}
SOLIS=./solis
[ -x "$SOLIS" ] || { echo "missing ./solis - run 'bl' first" >&2; exit 2; }

TMP=${TMPDIR:-/tmp}/solis-fuzz.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

bad=0
for ((seed = FIRST; seed < FIRST + COUNT; seed++)); do
    python3 tests/fuzz/gen.py "$seed" > "$TMP/p.sl"
    "$REF" "$TMP/p.sl" > "$TMP/want.txt" 2>&1
    "$SOLIS" --check-leaks "$TMP/p.sl" 2>&1 | grep -v "^live objects after run: 0$" > "$TMP/got.txt"
    if ! cmp -s "$TMP/want.txt" "$TMP/got.txt"; then
        bad=$((bad + 1))
        cp "$TMP/p.sl" "fuzz-fail-$seed.sl"
        echo "MISMATCH seed $seed (kept as fuzz-fail-$seed.sl)"
    fi
done
echo "$((COUNT - bad))/$COUNT programs agree"
[ "$bad" = 0 ]
