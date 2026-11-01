#!/bin/sh
# [UIT 0122] Builds and runs test_excl_preempt.c (Linux box test): a store-exclusive after a core switch fails.
# The runtime headers need clang (gcc cannot build them). CC picks the compiler; otherwise clang, then clang-NN on PATH.
set -e
cc=${CC:-}
if [ -z "$cc" ]; then
    for c in clang clang-23 clang-22 clang-21 clang-20 clang-19 clang-18; do
        if command -v "$c" >/dev/null 2>&1; then cc=$c; break; fi
    done
fi
if [ -z "$cc" ] || ! command -v "$cc" >/dev/null 2>&1; then
    echo "run_excl_preempt.sh: no clang found (put an LLVM bin folder on PATH or set CC=/path/to/clang); gcc cannot build the runtime headers" >&2
    exit 2
fi
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
"$cc" -O2 -march=x86-64-v3 -I src/runtime tools/tests/test_excl_preempt.c -o "$d/t"
"$d/t"
"$d/t" noclear   # control: without the clear the stale store succeeds
