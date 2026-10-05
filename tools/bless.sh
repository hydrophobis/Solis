#!/usr/bin/env bash
#
# Regenerate the golden bytecode and expected output in tests/.
#
# conform.sh compares against these, so run this only when a difference is
# intended, and read the diff before committing. Needs --yes: overwriting
# the baseline by accident is the one failure this can't survive.
#
#   tools/bless.sh --yes              regenerate everything
#   tools/bless.sh --yes hello        regenerate one
#
set -uo pipefail
cd "$(dirname "$0")/.."

. tools/examples.sh

confirmed=0
args=()
for a in "$@"; do
    if [ "$a" = "--yes" ]; then confirmed=1; else args+=("$a"); fi
done
if [ "$confirmed" != 1 ]; then
    echo "tools/bless.sh overwrites the baseline in tests/." >&2
    echo "Re-run with --yes if that is what you want." >&2
    exit 2
fi
set -- ${args[@]+"${args[@]}"}

SOLIS=./solis
[ -x "$SOLIS" ] || SOLIS=./solis.exe
[ -x "$SOLIS" ] || { echo "missing ./solis - run 'bl' first" >&2; exit 2; }

select_examples "$@" || exit 2

mkdir -p tests/golden tests/expected

for f in "${EXAMPLES[@]}"; do
    name=$(basename "$f" .sl)

    if ! "$SOLIS" -o "tests/golden/$name.slb" "$f" >/dev/null; then
        echo "FAIL $name: does not compile" >&2
        exit 1
    fi
    "$SOLIS" "tests/golden/$name.slb" 2>&1 | normalise > "tests/expected/$name.txt"

    bytes=$(wc -c < "tests/golden/$name.slb" | tr -d ' ')
    lines=$(wc -l < "tests/expected/$name.txt" | tr -d ' ')
    echo "blessed $name  ${bytes} bytes, ${lines} lines"
done
