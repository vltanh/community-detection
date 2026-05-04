#!/usr/bin/env bash
# Build the instrumented WCC kernel_check.
# Output: /tmp/wcc_kernel_check
#
# Mirrors the binary's link line at constrained-clustering/build/CMakeFiles
# /constrained_clustering.dir/link.txt (as of cmake build).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CC="$HERE/../../../../constrained-clustering"
EXT="$CC/external_libs"
INC_FLAGS=(
    "-I$EXT/include/igraph"
    "-I$EXT/include"
    "-I$CC/external_libs/VieCut/lib"
    "-I$CC/external_libs/VieCut/extlib/tlx"
    "-I$CC/external_libs/pcg-cpp/include"
    "-I$CC/includes"
    "-I$CC/src"
)
g++ -std=c++20 -O2 -Wall \
    "${INC_FLAGS[@]}" \
    -fopenmp \
    -o /tmp/wcc_kernel_check \
    "$HERE/kernel_check.cpp" \
    "$CC/build/libinternal_libs.a" \
    "$EXT/lib/liblibleidenalg.a" \
    "$EXT/lib/libigraph.a" \
    -lm -lz -lpthread -lxml2 \
    /usr/lib/x86_64-linux-gnu/libopenblas.so \
    /usr/lib/gcc/x86_64-linux-gnu/11/libgomp.so 2>/dev/null \
  || g++ -std=c++20 -O2 -Wall \
    "${INC_FLAGS[@]}" \
    -fopenmp \
    -o /tmp/wcc_kernel_check \
    "$HERE/kernel_check.cpp" \
    "$CC/build/libinternal_libs.a" \
    "$EXT/lib/liblibleidenalg.a" \
    "$EXT/lib/libigraph.a" \
    -lm -lz -lpthread -lxml2 -lopenblas -lgomp
echo "built: /tmp/wcc_kernel_check"
