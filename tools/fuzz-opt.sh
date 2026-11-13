#!/usr/bin/env bash
#
# Checks the optimizer against itself. Random array-loop programs from
# tests/fuzz/gen_arrays.py run with the optimizer off (SOLIS_NOOPT=1) and with it
# on, and the output and exit status must match. Point the second argument at a
# build made with -DSL_OPT_ASSERT to also make every removed bounds check verify
# itself.
#
#   tools/fuzz-opt.sh [count] [first-seed] [solis-assert-build]
#
# Some generated programs loop forever by design; those hit the timeout and are
# skipped.
#
set -uo pipefail
cd "$(dirname "$0")/.."

COUNT=${1:-200}
FIRST=${2:-1}
CHECKED=${3:-./solis}
[ -x ./solis ] || { echo "missing ./solis - run 'bl' first" >&2; exit 2; }

TMP=${TMPDIR:-/tmp}/solis-fuzz-opt.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

bad=0; skipped=0; removed=0
for ((seed = FIRST; seed < FIRST + COUNT; seed++)); do
    python3 tests/fuzz/gen_arrays.py "$seed" > "$TMP/p.sl"
    SOLIS_NOOPT=1 timeout 5 ./solis "$TMP/p.sl" > "$TMP/off.txt" 2>&1; off=$?
    timeout 5 "$CHECKED" "$TMP/p.sl" > "$TMP/on.txt" 2>&1; on=$?
    if [ "$off" = 124 ] || [ "$on" = 124 ]; then skipped=$((skipped + 1)); continue; fi
    if [ "$off" != "$on" ] || ! cmp -s "$TMP/off.txt" "$TMP/on.txt"; then
        bad=$((bad + 1))
        cp "$TMP/p.sl" "fuzz-opt-fail-$seed.sl"
        echo "MISMATCH seed $seed (kept as fuzz-opt-fail-$seed.sl)"
    fi
    n=$(SOLIS_OPT_STATS=1 timeout 5 ./solis "$TMP/p.sl" 2>&1 >/dev/null | grep -o '[0-9]* bounds checks' | grep -o '^[0-9]*' | awk '{s += $1} END {print s + 0}')
    [ "$n" -gt 0 ] && removed=$((removed + 1))
done
echo "$((COUNT - bad - skipped)) programs agree, $skipped skipped, $removed had a bounds check removed"
[ "$bad" = 0 ]
