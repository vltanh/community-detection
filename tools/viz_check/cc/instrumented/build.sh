#!/usr/bin/env bash
# Build the instrumented CC kernel_check.
#
# Skill-conformant: builds two binaries via -DTRACER_MODE / -DCANONICAL_MODE
# compile-time toggle. CC has no RNG/FP, so the toggle is vacuous (both
# binaries produce identical output). Toggle kept for skill conformance
# + future extension symmetry with VieCut/WCC/CM.
#
# Outputs:
#   /tmp/cc_kernel_check_canonical  (-DCANONICAL_MODE)
#   /tmp/cc_kernel_check_swapped    (-DTRACER_MODE)
#   /tmp/cc_kernel_check            (compat symlink → _canonical)
#
# Links against libigraph.a from constrained-clustering/external_libs (matches
# binary's igraph version exactly).
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

CXXFLAGS=(-std=c++20 -O2 -Wall -ffp-contract=off
          -I"$IGRAPH_INC" -I"$HERE/../../_common"
          "$HERE/kernel_check.cpp" "$IGRAPH_LIB"
          -lz -lpthread -lm -lxml2)

g++ -DCANONICAL_MODE "${CXXFLAGS[@]}" -o /tmp/cc_kernel_check_canonical
g++ -DTRACER_MODE    "${CXXFLAGS[@]}" -o /tmp/cc_kernel_check_swapped
ln -sf /tmp/cc_kernel_check_canonical /tmp/cc_kernel_check
echo "built: /tmp/cc_kernel_check_canonical (= /tmp/cc_kernel_check)"
echo "built: /tmp/cc_kernel_check_swapped"
