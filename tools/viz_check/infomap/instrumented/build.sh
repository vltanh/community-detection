#!/usr/bin/env bash
# Build the instrumented Infomap kernel_check.
# Output: /tmp/infomap_kernel_check
#
# Links against canonical Infomap's compiled .o set EXCEPT
# core/InfomapBase.o, which is replaced by infomap_base_traced.o
# (verbatim copy + per-stage [TRACE-IM] hooks inside InfomapBase::partition).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
IMSRC="$HERE/../../../../infomap/src"
IMBUILD="$HERE/../../../../infomap/build/Infomap"

if [[ ! -d "$IMBUILD" ]]; then
  echo "missing infomap build dir: $IMBUILD" >&2
  echo "run 'make' inside community-detection/infomap first" >&2
  exit 1
fi

CXXFLAGS=(-Wall -Wextra -pedantic -Wnon-virtual-dtor -std=c++14 -O3 -fopenmp
          -ffp-contract=off -fno-unsafe-math-optimizations -fno-fast-math -ffloat-store -fno-tree-vectorize)
INC=(-I"$IMSRC" -I"$IMSRC/core")
# Force-include traced infomath.h so its `#ifndef INFOMATH_H_` guard
# captures the macro before upstream's MapEquation.h pulls in the
# original. Every plogp call in re-compiled units now routes through
# jsmath::jsLog2 — bit-equal V8 Math.log2.
TRACE_INC=(-include "$HERE/infomath_traced.h")

g++ "${CXXFLAGS[@]}" "${INC[@]}" -c "$HERE/infomap_base_traced.cpp" \
    -o "$HERE/infomap_base_traced.o"
g++ "${CXXFLAGS[@]}" "${INC[@]}" -c "$HERE/main_traced.cpp" \
    -o "$HERE/main_traced.o"

# Recompile every upstream .cpp that instantiates MapEquation::update*
# / calcCodelength* templates, with infomath_traced.h force-included.
# These produce per-move plogp via jsLog2 instead of std::log2 — closing
# the 1-ulp drift between cpp running tracker + JS replay.
declare -a RECOMPILED_OBJS=()
for src in BiasedMapEquation MemMapEquation MetaMapEquation; do
  src_path="$IMSRC/core/$src.cpp"
  obj_path="$HERE/${src}_traced.o"
  g++ "${CXXFLAGS[@]}" "${INC[@]}" "${TRACE_INC[@]}" -c "$src_path" -o "$obj_path"
  RECOMPILED_OBJS+=("$obj_path")
done
# Recompile flow calculator + any other unit that calls plogp.
for src_path in "$IMSRC/utils/FlowCalculator.cpp"; do
  base=$(basename "$src_path" .cpp)
  obj_path="$HERE/${base}_traced.o"
  g++ "${CXXFLAGS[@]}" "${INC[@]}" "${TRACE_INC[@]}" -c "$src_path" -o "$obj_path"
  RECOMPILED_OBJS+=("$obj_path")
done

# Collect every Infomap .o EXCEPT core/InfomapBase.o (replaced),
# main.o (replaced), and the .o we just recompiled with traced plogp.
OBJS=()
while IFS= read -r o; do
  base="$(basename "$o")"
  rel="${o#"$IMBUILD/"}"
  if [[ "$rel" == "core/InfomapBase.o" || "$base" == "main.o" ]]; then
    continue
  fi
  if [[ "$base" == "BiasedMapEquation.o" || "$base" == "MemMapEquation.o" \
        || "$base" == "MetaMapEquation.o" || "$base" == "FlowCalculator.o" ]]; then
    continue
  fi
  OBJS+=("$o")
done < <(find "$IMBUILD" -name "*.o" | sort)

g++ "${CXXFLAGS[@]}" -fopenmp -o /tmp/infomap_kernel_check \
    "$HERE/main_traced.o" \
    "$HERE/infomap_base_traced.o" \
    "${RECOMPILED_OBJS[@]}" \
    "${OBJS[@]}"
echo "built: /tmp/infomap_kernel_check"
