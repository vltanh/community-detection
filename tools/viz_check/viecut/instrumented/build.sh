#!/usr/bin/env bash
# Build the instrumented VieCut tracer. Output: /tmp/viecut_traced
#
# Mirrors VieCut's mincut binary build but routes the random_functions
# include through the instrumented sibling header (which emits [TRACE-RNG]
# stderr lines on every RNG call).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
VC="$HERE/../../../../VieCut"
INSTRUMENTED_INC="$HERE/include"

INC_FLAGS=(
    "-I$INSTRUMENTED_INC"          # instrumented headers FIRST
    "-I$VC/lib"
    "-I$VC/extlib/tlx"
    "-I$VC/extlib/growt"
    "-isystem" "/usr/lib/x86_64-linux-gnu/openmpi/include"
)

OPT_FLAGS=(
    -std=gnu++17 -O2 -Wall -Wextra
    -DUSE_TCMALLOC
    -DVIECUT_PATH=\"$VC\"
)

g++ "${OPT_FLAGS[@]}" "${INC_FLAGS[@]}" \
    -fopenmp \
    -o /tmp/viecut_traced \
    "$HERE/main_traced.cpp" \
    "$VC/build/extlib/tlx/tlx/libtlx.a" \
    /usr/lib/x86_64-linux-gnu/libtcmalloc.so \
    /usr/lib/gcc/x86_64-linux-gnu/11/libgomp.so \
    -lpthread

echo "built: /tmp/viecut_traced"
