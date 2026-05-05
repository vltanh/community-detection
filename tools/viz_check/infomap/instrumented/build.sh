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
          -ffp-contract=off -fno-unsafe-math-optimizations)
INC=(-I"$IMSRC" -I"$IMSRC/core")

g++ "${CXXFLAGS[@]}" "${INC[@]}" -c "$HERE/infomap_base_traced.cpp" \
    -o "$HERE/infomap_base_traced.o"
g++ "${CXXFLAGS[@]}" "${INC[@]}" -c "$HERE/main_traced.cpp" \
    -o "$HERE/main_traced.o"

# Collect every Infomap .o EXCEPT core/InfomapBase.o (replaced) +
# the canonical main.o (replaced).
OBJS=()
while IFS= read -r o; do
  base="$(basename "$o")"
  rel="${o#"$IMBUILD/"}"
  if [[ "$rel" == "core/InfomapBase.o" || "$base" == "main.o" ]]; then
    continue
  fi
  OBJS+=("$o")
done < <(find "$IMBUILD" -name "*.o" | sort)

g++ "${CXXFLAGS[@]}" -fopenmp -o /tmp/infomap_kernel_check \
    "$HERE/main_traced.o" \
    "$HERE/infomap_base_traced.o" \
    "${OBJS[@]}"
echo "built: /tmp/infomap_kernel_check"
