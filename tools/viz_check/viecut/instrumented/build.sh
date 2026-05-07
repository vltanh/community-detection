#!/usr/bin/env bash
# Build the instrumented VieCut tracer.
#
# Skill-conformant compile-time toggle (per byte-equal-tracer playbook §1):
#   -DCANONICAL_MODE -> /tmp/viecut_traced_canonical
#                       std::unordered_set/map at row-H sites; bit-equal
#                       to unmodified VieCut/build/mincut for build-pair
#                       test (a).
#   -DTRACER_MODE    -> /tmp/viecut_traced_swapped
#                       TracerSet/TracerMap aliased to std::set/std::map
#                       (id-ASC iteration); matches JS port's
#                       sort-on-demand row-H closure.
#
# Compatibility symlink: /tmp/viecut_traced -> _canonical.
#
# Mirrors VieCut's mincut binary build but routes the random_functions
# include + container_swap through the instrumented sibling headers
# (which take precedence in the include search path).
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

CXXFLAGS=(
    -std=gnu++17 -O2 -Wall -Wextra -ffp-contract=off
    -DUSE_TCMALLOC
    -DVIECUT_PATH=\"$VC\"
)

LINK_FLAGS=(
    -fopenmp
    "$HERE/main_traced.cpp"
    "$VC/build/extlib/tlx/tlx/libtlx.a"
    /usr/lib/x86_64-linux-gnu/libtcmalloc.so
    /usr/lib/gcc/x86_64-linux-gnu/11/libgomp.so
    -lpthread
)

g++ -DCANONICAL_MODE "${CXXFLAGS[@]}" "${INC_FLAGS[@]}" \
    -o /tmp/viecut_traced_canonical "${LINK_FLAGS[@]}"

g++ -DTRACER_MODE "${CXXFLAGS[@]}" "${INC_FLAGS[@]}" \
    -o /tmp/viecut_traced_swapped "${LINK_FLAGS[@]}"

ln -sf /tmp/viecut_traced_canonical /tmp/viecut_traced

echo "built: /tmp/viecut_traced_canonical (= /tmp/viecut_traced)"
echo "built: /tmp/viecut_traced_swapped"
