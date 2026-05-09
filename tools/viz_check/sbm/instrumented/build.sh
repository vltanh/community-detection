#!/usr/bin/env bash
# Build the SBM flat-variant kernel-tracer in two modes (skill:
# byte-equal-tracer / Compile-time tracer-mode toggle):
#
#   /tmp/sbm_flat_kernel_check           = TRACER_MODE   (V8-bit-equiv math)
#   /tmp/sbm_flat_kernel_check_canonical = CANONICAL_MODE (libm std::*)
#
# Both binaries share one source (flat_traced.cpp); the `-D` macro
# selects whether entropy/DL math routes through jsLog/jsExp/jsLgamma
# (TRACER_MODE: byte-IDENTICAL vs JS Math.*) or through std::log/exp/lgamma
# (CANONICAL_MODE: libm; ~1ulp drift vs V8 — used for build-pair test +
# structural agreement).
#
# RNG is std::mt19937 in BOTH modes. graph-tool's pcg64_k1024 is a
# sanctioned divergence (see codebase_map.md "Layered canonical flag");
# the SBM canonical-tracer is therefore not a drop-in replacement for
# graph-tool — it is a fork that uses MT19937 to share an RNG family
# with the JS port. Test (a) build-pair vs unmodified graph-tool is N/A;
# tests (b) self-determinism + (c) structural still apply.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
COMMON=( -std=c++20 -O2 -ffp-contract=off -Wall -Wno-unused-result )
g++ "${COMMON[@]}" -DTRACER_MODE \
    -o /tmp/sbm_flat_kernel_check "$HERE/flat_traced.cpp"
g++ "${COMMON[@]}" -DCANONICAL_MODE \
    -o /tmp/sbm_flat_kernel_check_canonical "$HERE/flat_traced.cpp"
echo "built: /tmp/sbm_flat_kernel_check (TRACER_MODE)"
echo "built: /tmp/sbm_flat_kernel_check_canonical (CANONICAL_MODE)"
