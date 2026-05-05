#!/usr/bin/env bash
# Build the L4 Louvain bit-equal-per-step tracer.
# Single-file, self-contained (does NOT link canonical externals/louvain/.o).
# Output: /tmp/louvain_l4_tracer
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
g++ -O2 -ffp-contract=off -std=c++17 -Wall \
    "$HERE/louvain_l4_tracer.cpp" \
    -o /tmp/louvain_l4_tracer
echo "built: /tmp/louvain_l4_tracer"
