#!/usr/bin/env bash
# Build the L4 Louvain bit-equal-per-step tracer in BOTH modes.
#
# Single source `louvain_l4_tracer.cpp` produces two binaries via the
# byte-equal-tracer skill's compile-time mode toggle:
#
#   /tmp/louvain_l4_tracer_swapped    (-DLV_TRACER_MODE)
#       MT19937 + double; the JS-bit-equal cross-check artifact.
#       Paired with kernel_check_l4.mjs (self-RNG end-to-end).
#
#   /tmp/louvain_l4_tracer_canonical  (-DLV_CANONICAL_MODE)
#       libc rand + long double; matches gen-louvain externals/louvain
#       under matching LD_PRELOAD seed shim. For the build-pair
#       equivalence test (kernel_check_l4.py canonical-build leg).
#
# Back-compat: /tmp/louvain_l4_tracer is symlinked to the swapped
# binary so callers that hardcoded the old path still work.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/louvain_l4_tracer.cpp"

CFLAGS=(-O2 -ffp-contract=off -std=c++17 -Wall)

g++ "${CFLAGS[@]}" -DLV_TRACER_MODE   "$SRC" -o /tmp/louvain_l4_tracer_swapped
g++ "${CFLAGS[@]}" -DLV_CANONICAL_MODE "$SRC" -o /tmp/louvain_l4_tracer_canonical

# Back-compat: legacy callers expect /tmp/louvain_l4_tracer (= swapped).
ln -sf /tmp/louvain_l4_tracer_swapped /tmp/louvain_l4_tracer

echo "built:"
echo "  /tmp/louvain_l4_tracer_swapped    (LV_TRACER_MODE)"
echo "  /tmp/louvain_l4_tracer_canonical  (LV_CANONICAL_MODE)"
echo "  /tmp/louvain_l4_tracer            -> /tmp/louvain_l4_tracer_swapped (back-compat)"
