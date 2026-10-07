# Shared by conform.sh and bless.sh, so the two can never drift over which
# examples are checked or how their output is normalised.
#
# Not executable on its own; source it.

EXAMPLES=(
    examples/hello.sl
    examples/conform.sl
    examples/unicode.sl
    examples/array.sl
    examples/bench.sl
    examples/generics.sl
    examples/generic_funcs.sl
    examples/map_demo.sl
    examples/option_result.sl
    examples/slices.sl
    examples/test_demo.sl
    examples/stdlib.sl
    examples/host.sl
    examples/native.sl
    examples/app/main.sl
)

# Narrow EXAMPLES to one named example, if an argument was given.
select_examples() {
    [ $# -gt 0 ] || return 0
    local want=$1 picked=() f
    for f in "${EXAMPLES[@]}"; do
        [ "$(basename "$f" .sl)" = "$want" ] && picked+=("$f")
    done
    if [ ${#picked[@]} -eq 0 ]; then
        echo "no example named '$want'" >&2
        return 1
    fi
    EXAMPLES=("${picked[@]}")
}

# A wall-clock time and the host's own name differ between runs and between
# hosts by design, so both are erased before anything is compared or stored.
normalise() {
    sed -E 's/[0-9]+ ms/N ms/; s/solis-cli/HOST/; s/solis-c$/HOST/'
}
