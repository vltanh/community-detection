#!/usr/bin/env bash
# Build the instrumented Louvain kernel_check.
# Output: /tmp/louvain_kernel_check
#
# Links against gen-louvain's compiled .o files (everything except
# louvain.o, which we override with louvain_traced.o).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
GLV="$HERE/../../../../externals/louvain/src"

CXXFLAGS=(-O2 -Wall -std=c++17)
INC=(-I"$GLV")

# Compile the traced louvain + main.
g++ "${CXXFLAGS[@]}" "${INC[@]}" -c "$HERE/louvain_traced.cpp" -o "$HERE/louvain_traced.o"
g++ "${CXXFLAGS[@]}" "${INC[@]}" -c "$HERE/main_traced.cpp" -o "$HERE/main_traced.o"

# Link with all gen-louvain .o files except louvain.o (replaced).
OBJS=(
  "$GLV/graph_binary.o"
  "$HERE/louvain_traced.o"
  "$GLV/quality.o"
  "$GLV/modularity.o"
  "$GLV/zahn.o"
  "$GLV/owzad.o"
  "$GLV/goldberg.o"
  "$GLV/condora.o"
  "$GLV/devind.o"
  "$GLV/devuni.o"
  "$GLV/dp.o"
  "$GLV/shimalik.o"
  "$GLV/balmod.o"
  "$HERE/main_traced.o"
)
g++ -o /tmp/louvain_kernel_check "${OBJS[@]}"
echo "built: /tmp/louvain_kernel_check"
