#!/usr/bin/env bash
#
# Builds tests/api/api_test.c against the compiler and runtime libraries, using
# only their public headers, and runs it.
#
#   tools/apitest.sh
#
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${TMPDIR:-/tmp}/solis-apitest.$$
trap 'rm -f "$OUT"' EXIT

CC=${CC:-gcc}
for t in api_test analysis_test; do
    $CC -std=c23 -O1 -g -Wall -Wextra -Icompiler -Iruntime \
        tests/api/$t.c compiler/*.c \
        runtime/solis.c runtime/solis_std.c runtime/solis_os.c \
        -o "$OUT" -lm
    "$OUT"
done
