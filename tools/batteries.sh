#!/usr/bin/env bash
#
# Checks what conformance can't: the batteries (io, fs, os, time, rand) and
# the prelude. A clock and a filesystem are the wrong thing to hold a
# byte-identical oracle to, so they stay outside tools/conform.sh.
# tests/batteries.sl asserts in Solis instead; this script gives it a
# scratch dir and empty stdin, then checks the prelude example separately.
#
#   tools/batteries.sh
#
set -uo pipefail
cd "$(dirname "$0")/.."

SOLIS=./solis
[ -x "$SOLIS" ] || SOLIS=./solis.exe
[ -x "$SOLIS" ] || { echo "missing ./solis - run 'bl' first" >&2; exit 2; }

BASE=${TMPDIR:-/tmp}/solis-batteries.$$
DIR=$BASE/scratch
rm -rf "$BASE"
mkdir -p "$DIR" || exit 2
trap 'rm -rf "$BASE"' EXIT

# Empty stdin, because the io checks test the end-of-input behaviour. Both
# ways of running it, because interpreting and compiling have to agree.
"$SOLIS" "$(pwd)/tests/batteries.sl" "$DIR" < /dev/null || exit 1

# The image goes beside the scratch directory, not in it: the fs checks count
# what is in there.
"$SOLIS" -o "$BASE/batteries.slb" tests/batteries.sl > /dev/null || exit 1
"$SOLIS" "$BASE/batteries.slb" "$DIR" < /dev/null || exit 1
echo "batteries: ok compiled too"

# The prelude: examples/plugins/mod.sl declares no externs at all and reaches
# the host through prelude.sl beside it. With --no-prelude the same file must
# stop compiling, or the mechanism is not doing anything.
cat > "$BASE/want.txt" <<'WANT'
plugin running on solis-c
------------------------
LOADED!
repeat: ababab
budget: 16 ms
the clock is reachable: true
WANT

"$SOLIS" examples/plugins/mod.sl > "$BASE/got.txt" 2>&1
if ! diff -q --strip-trailing-cr "$BASE/want.txt" "$BASE/got.txt" >/dev/null; then
    echo "FAIL prelude: examples/plugins/mod.sl printed" >&2
    diff --strip-trailing-cr "$BASE/want.txt" "$BASE/got.txt" | sed 's/^/      /' >&2
    exit 1
fi
if "$SOLIS" --no-prelude --check examples/plugins/mod.sl >/dev/null 2>&1; then
    echo "FAIL prelude: --no-prelude still resolved the host declarations" >&2
    exit 1
fi
echo "prelude: ok, and --no-prelude takes it away"
