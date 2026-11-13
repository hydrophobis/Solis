#!/usr/bin/env bash
# Solis vs C wall time per workload. Best of N runs.
#   benchmarks/run.sh [runs] [solis-binary]
cd "$(dirname "$0")"
RUNS=${1:-5}
SOLIS=${2:-../solis}
OUT=${TMPDIR:-/tmp}/solis-bench
mkdir -p "$OUT"

best() { # best <cmd...>: minimum wall seconds over $RUNS runs
    local m=999999999 t s e
    for _ in $(seq "$RUNS"); do
        s=$(date +%s%N); "$@" >/dev/null; e=$(date +%s%N)
        t=$(( (e - s) / 1000 )); (( t < m )) && m=$t
    done
    echo "$m"
}

printf "%-10s %10s %10s %10s %10s %8s\n" bench "solis ms" "image ms" "C ms" "lua5.4 ms" "vs C"
for src in sl/*.sl; do
    n=$(basename "$src" .sl)
    gcc -O2 -o "$OUT/$n" "c/$n.c" || continue
    "$SOLIS" -o "$OUT/$n.slb" "$src" >/dev/null || continue
    a=$(best "$SOLIS" "$src")
    b=$(best "$SOLIS" "$OUT/$n.slb")
    c=$(best "$OUT/$n")
    l=0; command -v lua5.4 >/dev/null && [ -f "lua/$n.lua" ] && l=$(best lua5.4 "lua/$n.lua")
    awk -v n="$n" -v a="$a" -v b="$b" -v c="$c" -v l="$l" \
        'BEGIN { printf "%-10s %10.1f %10.1f %10.1f %10.1f %7.0fx\n", n, a/1000, b/1000, c/1000, l/1000, (c > 0 ? b/c : 0) }'
done
