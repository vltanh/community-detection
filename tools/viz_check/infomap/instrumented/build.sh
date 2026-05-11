#!/usr/bin/env bash
# Build the instrumented Infomap kernel_check pair (skill §"Compile-time
# tracer-mode toggle"):
#
#   /tmp/infomap_kernel_check_canonical (-DCANONICAL_MODE)
#       plogp -> std::log2; trace prints intact. Used for the build-pair
#       equivalence test: this binary's emitted com.csv must bit-equal the
#       unmodified canonical pipeline's output under the same seed.
#   /tmp/infomap_kernel_check_swapped   (-DTRACER_MODE)
#       plogp -> jsmath::jsLog2 (fdlibm e_log * LOG2E, V8-bit-equivalent).
#       Verification target the JS production walker cross-checks against.
#
# A symlink /tmp/infomap_kernel_check -> *_swapped is maintained for
# legacy callers (kernel_check.py, batch_empirical.py, visit_decision_diff.mjs).
#
# Links against canonical Infomap's compiled .o set EXCEPT
# core/InfomapBase.o (replaced by infomap_base_traced.o), main.o (replaced
# by main_traced.o), and the four .o units that instantiate plogp templates
# (recompiled with the toggled infomath_traced.h force-included).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
IMSRC="$HERE/../../../../infomap/src"
IMBUILD="$HERE/../../../../infomap/build/Infomap"

if [[ ! -d "$IMBUILD" ]]; then
  echo "missing infomap build dir: $IMBUILD" >&2
  echo "run 'make' inside community-detection/infomap first" >&2
  exit 1
fi

CXXFLAGS_BASE=(-Wall -Wextra -pedantic -Wnon-virtual-dtor -std=c++14 -O3 -fopenmp
               -ffp-contract=off -fno-unsafe-math-optimizations -fno-fast-math
               -ffloat-store -fno-tree-vectorize)
INC=(-I"$IMSRC" -I"$IMSRC/core")
TRACE_INC=(-include "$HERE/infomath_traced.h"
           -include "$HERE/map_equation_traced.h")

build_one() {
  local mode="$1"             # CANONICAL_MODE or TRACER_MODE
  local out_path="$2"         # final binary path
  local stamp="${mode,,}"     # lowercase for object suffix

  local CXXFLAGS=("${CXXFLAGS_BASE[@]}" "-D${mode}")

  g++ "${CXXFLAGS[@]}" "${INC[@]}" -c "$HERE/infomap_base_traced.cpp" \
      -o "$HERE/infomap_base_traced.${stamp}.o"
  g++ "${CXXFLAGS[@]}" "${INC[@]}" -c "$HERE/main_traced.cpp" \
      -o "$HERE/main_traced.${stamp}.o"

  # Recompile every upstream .cpp that instantiates MapEquation::update*
  # / calcCodelength* templates, with infomath_traced.h force-included
  # so plogp inside those template bodies resolves to the toggled backend.
  declare -a RECOMPILED_OBJS=()
  for src in BiasedMapEquation MemMapEquation MetaMapEquation; do
    local src_path="$IMSRC/core/$src.cpp"
    local obj_path="$HERE/${src}_traced.${stamp}.o"
    g++ "${CXXFLAGS[@]}" "${INC[@]}" "${TRACE_INC[@]}" -c "$src_path" -o "$obj_path"
    RECOMPILED_OBJS+=("$obj_path")
  done
  for src_path in "$IMSRC/utils/FlowCalculator.cpp"; do
    local base
    base=$(basename "$src_path" .cpp)
    local obj_path="$HERE/${base}_traced.${stamp}.o"
    g++ "${CXXFLAGS[@]}" "${INC[@]}" "${TRACE_INC[@]}" -c "$src_path" -o "$obj_path"
    RECOMPILED_OBJS+=("$obj_path")
  done

  # Collect every Infomap .o EXCEPT core/InfomapBase.o (replaced),
  # main.o (replaced), and the .o we just recompiled with toggled plogp.
  local OBJS=()
  while IFS= read -r o; do
    local base rel
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

  g++ "${CXXFLAGS[@]}" -fopenmp -o "$out_path" \
      "$HERE/main_traced.${stamp}.o" \
      "$HERE/infomap_base_traced.${stamp}.o" \
      "${RECOMPILED_OBJS[@]}" \
      "${OBJS[@]}"
  echo "built: $out_path  (-D${mode})"
}

build_one CANONICAL_MODE /tmp/infomap_kernel_check_canonical
build_one TRACER_MODE    /tmp/infomap_kernel_check_swapped

# Legacy symlink so existing harnesses keep working.
ln -sfn /tmp/infomap_kernel_check_swapped /tmp/infomap_kernel_check
echo "symlink: /tmp/infomap_kernel_check -> /tmp/infomap_kernel_check_swapped"
