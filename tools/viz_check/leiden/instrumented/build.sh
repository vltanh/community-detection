#!/usr/bin/env bash
# Build the instrumented Leiden kernel_check (byte-equal trace).
#
# Links against libleidenalg's compiled .o files EXCEPT Optimiser.cpp.o,
# which is replaced by the forked optimiser_traced.cpp.o (verbatim copy
# with [TRACE-LD] log + capture in move_nodes).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CC="$HERE/../../../../constrained-clustering"
EXT="$CC/external_libs"
LD_BUILD="$EXT/libleidenalg/build/src/CMakeFiles/libleidenalg.dir"
INC=(
    "-I$EXT/include/igraph"
    "-I$EXT/include"
    "-I$CC/external_libs/VieCut/lib"
    "-I$CC/external_libs/VieCut/extlib/tlx"
    "-I$CC/external_libs/pcg-cpp/include"
    "-I$EXT/libleidenalg/include"
    "-I$EXT/libleidenalg/build/include"
    "-I$CC/includes"
    "-I$CC/src"
    "-I$HERE/../../_common"
)

# Compile the forked Optimiser + MutableVertexPartition + CPM/Mod +
# traced main. -ffp-contract=off prevents gcc from FMA-fusing diff_move's
# `nv*(2.0*csize - nv - 1.0)` into a single-rounding fma; JS V8 does
# not fma, so contract-off is required for bit-equal dQ.
g++ -std=c++20 -O2 -Wall -ffp-contract=off "${INC[@]}" -fopenmp \
    -c "$HERE/optimiser_traced.cpp" -o "$HERE/optimiser_traced.o"
g++ -std=c++20 -O2 -Wall -ffp-contract=off "${INC[@]}" -fopenmp \
    -c "$HERE/mutable_vertex_partition_traced.cpp" -o "$HERE/mutable_vertex_partition_traced.o"
g++ -std=c++20 -O2 -Wall -ffp-contract=off "${INC[@]}" -fopenmp \
    -c "$HERE/cpm_vertex_partition_traced.cpp" -o "$HERE/cpm_vertex_partition_traced.o"
g++ -std=c++20 -O2 -Wall -ffp-contract=off "${INC[@]}" -fopenmp \
    -c "$HERE/modularity_vertex_partition_traced.cpp" -o "$HERE/modularity_vertex_partition_traced.o"
g++ -std=c++20 -O2 -Wall -ffp-contract=off "${INC[@]}" -fopenmp \
    -c "$HERE/main_traced.cpp" -o "$HERE/main_traced.o"

# Collect every libleidenalg .o EXCEPT the canonical Optimiser.cpp.o,
# MutableVertexPartition.cpp.o, CPMVertexPartition.cpp.o, and
# ModularityVertexPartition.cpp.o (all replaced by tracer-forked copies
# with per-move admin + cache_neigh_communities + per-call diff_move
# term-by-term probe emissions).
LD_OBJS=()
for o in "$LD_BUILD"/*.o; do
  base=$(basename "$o")
  if [[ "$base" == "Optimiser.cpp.o" ]]; then continue; fi
  if [[ "$base" == "MutableVertexPartition.cpp.o" ]]; then continue; fi
  if [[ "$base" == "CPMVertexPartition.cpp.o" ]]; then continue; fi
  if [[ "$base" == "ModularityVertexPartition.cpp.o" ]]; then continue; fi
  LD_OBJS+=("$o")
done

g++ -std=c++20 -O2 -Wall \
    -fopenmp \
    -o /tmp/leiden_kernel_check \
    "$HERE/main_traced.o" \
    "$HERE/optimiser_traced.o" \
    "$HERE/mutable_vertex_partition_traced.o" \
    "$HERE/cpm_vertex_partition_traced.o" \
    "$HERE/modularity_vertex_partition_traced.o" \
    "${LD_OBJS[@]}" \
    "$CC/build/libinternal_libs.a" \
    "$EXT/lib/libigraph.a" \
    -lm -lz -lpthread -lxml2 -lopenblas -lgomp
echo "built: /tmp/leiden_kernel_check"
