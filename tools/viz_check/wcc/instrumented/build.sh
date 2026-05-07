#!/usr/bin/env bash
# Build the instrumented WCC kernel_check.
#
# Skill-conformant compile-time toggle (per byte-equal-tracer playbook §1):
#   -DCANONICAL_MODE -> /tmp/wcc_kernel_check_canonical
#                       std::log + canonical VieCut chain (std::unordered_*).
#                       For build-pair test (a) vs unmodified canonical_clustering.
#   -DTRACER_MODE    -> /tmp/wcc_kernel_check_swapped
#                       jsLog (V8-equivalent fdlibm port) + TRACER_MODE-
#                       swapped VieCut chain (std::set/map row-H closure).
#                       Cross-check artifact for L4 self-RNG vs JS production walker.
#
# Compatibility symlink: /tmp/wcc_kernel_check -> _canonical.
#
# VieCut headers are picked up from tools/viz_check/viecut/instrumented/include/
# FIRST in the include path, so the CANONICAL_MODE/TRACER_MODE swap propagates
# into all VieCut templates instantiated here. MinCutCustom is inlined into
# kernel_check.cpp (mincut_custom.cpp:3-107) since libinternal_libs.a can't
# be retrofitted with the toggle.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CC="$HERE/../../../../constrained-clustering"
EXT="$CC/external_libs"
VC="$HERE/../../../../VieCut"
VC_INSTRUMENTED="$HERE/../../viecut/instrumented/include"

INC_FLAGS=(
    "-I$VC_INSTRUMENTED"          # instrumented VieCut sibling headers FIRST
    "-I$VC/lib"
    "-I$VC/extlib/tlx"
    "-I$VC/extlib/growt"
    "-I$EXT/include/igraph"
    "-I$EXT/include"
    "-I$CC/includes"
    "-I$CC/src"
    "-I$HERE/../../_common"
    "-isystem" "/usr/lib/x86_64-linux-gnu/openmpi/include"
)

CXXFLAGS=(
    -std=gnu++20 -O2 -Wall -ffp-contract=off
    -DUSE_TCMALLOC
    -DVIECUT_PATH=\"$VC\"
)

# LINK_FLAGS_COMMON: shared between both modes.
LINK_FLAGS_COMMON=(
    -fopenmp
    "$HERE/kernel_check.cpp"
    "$VC/build/extlib/tlx/tlx/libtlx.a"
    "$EXT/lib/libigraph.a"
    /usr/lib/x86_64-linux-gnu/libtcmalloc.so
    /usr/lib/gcc/x86_64-linux-gnu/11/libgomp.so
    -lz -lpthread -lm -lxml2
)

# CANONICAL_MODE pulls libinternal_libs.a so MinCutCustom's RNG static
# state lives in the same TU as the unmodified canonical pipeline →
# build-pair test (a) bit-equal. libleidenalg + openblas needed by
# libinternal_libs.a's transitive symbols.
g++ -DCANONICAL_MODE "${CXXFLAGS[@]}" "${INC_FLAGS[@]}" \
    -o /tmp/wcc_kernel_check_canonical \
    "${LINK_FLAGS_COMMON[@]}" \
    "$CC/build/libinternal_libs.a" \
    "$EXT/lib/liblibleidenalg.a" \
    /usr/lib/x86_64-linux-gnu/libopenblas.so

# TRACER_MODE inlines MinCutInline locally; container_swap.h aliases
# TracerSet=std::set so signatures DIFFER from libinternal_libs.a's
# canonical compilation (which uses unordered_set). Don't link
# libinternal_libs.a — random_functions::m_mt static lives in this TU.
g++ -DTRACER_MODE "${CXXFLAGS[@]}" "${INC_FLAGS[@]}" \
    -o /tmp/wcc_kernel_check_swapped \
    "${LINK_FLAGS_COMMON[@]}"

ln -sf /tmp/wcc_kernel_check_canonical /tmp/wcc_kernel_check

echo "built: /tmp/wcc_kernel_check_canonical (= /tmp/wcc_kernel_check)"
echo "built: /tmp/wcc_kernel_check_swapped"
