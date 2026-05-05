#!/usr/bin/env bash
# Build the instrumented CM kernel_check. Same link surface as WCC's,
# plus libleidenalg headers + libleidenalg.a for the recluster step.
# Output: /tmp/cm_kernel_check
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CC="$HERE/../../../../constrained-clustering"
EXT="$CC/external_libs"
INC=(
    "-I$EXT/include/igraph"
    "-I$EXT/include"
    "-I$CC/external_libs/VieCut/lib"
    "-I$CC/external_libs/VieCut/extlib/tlx"
    "-I$CC/external_libs/pcg-cpp/include"
    "-I$CC/includes"
    "-I$CC/src"
    "-I$HERE/../../_common"
)
g++ -std=c++20 -O2 -Wall \
    "${INC[@]}" \
    -fopenmp \
    -o /tmp/cm_kernel_check \
    "$HERE/kernel_check.cpp" \
    "$CC/build/libinternal_libs.a" \
    "$EXT/lib/liblibleidenalg.a" \
    "$EXT/lib/libigraph.a" \
    -lm -lz -lpthread -lxml2 -lopenblas -lgomp
echo "built: /tmp/cm_kernel_check"
