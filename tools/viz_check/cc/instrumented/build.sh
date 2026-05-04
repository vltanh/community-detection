#!/usr/bin/env bash
# Build the instrumented CC kernel_check.
# Output binary: /tmp/cc_kernel_check
#
# Links against the libigraph.a shipped with constrained-clustering's
# external_libs (matching the binary's igraph version exactly).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
EXT="$HERE/../../../../constrained-clustering/external_libs"
IGRAPH_INC="$EXT/include/igraph"
IGRAPH_LIB="$EXT/lib/libigraph.a"
if [[ ! -f "$IGRAPH_LIB" ]]; then
    IGRAPH_LIB="$EXT/igraph/build/src/libigraph.a"
fi
if [[ ! -f "$IGRAPH_LIB" ]]; then
    echo "ERROR: cannot find libigraph.a; tried:" 1>&2
    echo "  $EXT/lib/libigraph.a" 1>&2
    echo "  $EXT/igraph/build/src/libigraph.a" 1>&2
    exit 2
fi
g++ -std=c++20 -O2 -Wall \
    -I"$IGRAPH_INC" \
    -o /tmp/cc_kernel_check \
    "$HERE/kernel_check.cpp" \
    "$IGRAPH_LIB" \
    -lz -lpthread -lm -lxml2
echo "built: /tmp/cc_kernel_check"
