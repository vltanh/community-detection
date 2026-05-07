#!/usr/bin/env bash
# Build the instrumented CM kernel_check with -DCANONICAL_MODE/-DTRACER_MODE
# compile-time toggle per byte-equal-tracer skill playbook §1.
#
# Outputs:
#   /tmp/cm_kernel_check_canonical  (-DCANONICAL_MODE; bit-equal unmodified
#                                    constrained_clustering CM ...)
#   /tmp/cm_kernel_check_swapped    (-DTRACER_MODE; jsLog + std::set/map row-H
#                                    swap; bit-equal JS production walker)
#   /tmp/cm_kernel_check            (compat symlink -> _canonical)
#
# VieCut chain: kernel_check.cpp INLINES MinCutCustom (replacing the
# libinternal_libs.a wrapper) so the VieCut call path picks up the
# instrumented sibling headers via -I order. The TRACER_MODE container_swap
# (std::unordered_set -> std::set under TRACER_MODE) propagates through
# mutable_graph + heavy_edges + contract_graph + recursive_cactus.
#
# Leiden chain: stays canonical (libleidenalg.a + libigraph.a). Per Leiden
# audit row A-M, JS Leiden is L4 bit-equal under canonical libleidenalg +
# igraph_rng. No TRACER_MODE swap needed on Leiden side.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CC="$HERE/../../../../constrained-clustering"
EXT="$CC/external_libs"
VIECUT_TRACER="$HERE/../../viecut/instrumented"

# -I order matters: VieCut instrumented sibling headers FIRST (so
# container_swap.h and the eight row-H sibling forks take precedence over
# upstream VieCut/lib/* headers). _common SECOND for js_math_port.h. Then
# upstream VieCut, then constrained-clustering, then external_libs.
INC=(
    "-I$VIECUT_TRACER/include"
    "-I$HERE/../../_common"
    "-I$CC/external_libs/VieCut/lib"
    "-I$CC/external_libs/VieCut/extlib/tlx"
    "-I$EXT/include/igraph"
    "-I$EXT/include"
    "-I$CC/external_libs/pcg-cpp/include"
    "-I$CC/includes"
    "-I$CC/src"
)

# -ffp-contract=off per audit row E (prevents FMA fusion of pre_log * log(n)).
VC_TLX="$HERE/../../../../VieCut/build/extlib/tlx/tlx/libtlx.a"
if [[ ! -f "$VC_TLX" ]]; then
    echo "ERROR: cannot find libtlx.a at $VC_TLX" 1>&2
    exit 2
fi
CXXFLAGS=(-std=c++20 -O2 -Wall -ffp-contract=off "${INC[@]}" -fopenmp
          "$HERE/kernel_check.cpp"
          "$EXT/lib/liblibleidenalg.a"
          "$EXT/lib/libigraph.a"
          "$VC_TLX"
          -lm -lz -lpthread -lxml2)

build_one() {
    local mode="$1"; local outfile="$2"
    g++ "-D${mode}" "${CXXFLAGS[@]}" -o "$outfile" \
        /usr/lib/x86_64-linux-gnu/libopenblas.so \
        /usr/lib/gcc/x86_64-linux-gnu/11/libgomp.so 2>/dev/null \
      || g++ "-D${mode}" "${CXXFLAGS[@]}" -o "$outfile" -lopenblas -lgomp
}

build_one CANONICAL_MODE /tmp/cm_kernel_check_canonical
build_one TRACER_MODE    /tmp/cm_kernel_check_swapped
ln -sf /tmp/cm_kernel_check_canonical /tmp/cm_kernel_check
echo "built: /tmp/cm_kernel_check_canonical (= /tmp/cm_kernel_check)"
echo "built: /tmp/cm_kernel_check_swapped"
