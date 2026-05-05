#!/usr/bin/env bash
# Build the SBM flat-variant kernel-tracer. Output binary lives at
# /tmp/sbm_flat_kernel_check; per-variant kernel_check.py drivers run
# this binary with --mode={dc,ndc,pp}.
#
# Per-leg semantics (see flat_traced.cpp header): instrumented C++ uses
# std::mt19937 (NOT graph-tool's pcg64_k1024); JS replay byte-equal vs
# this binary, while structural agreement vs canonical graph-tool is a
# separate Python-side entropy-at-fixed-partition check.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
g++ -std=c++20 -O2 -ffp-contract=off -Wall -Wno-unused-result \
    -o /tmp/sbm_flat_kernel_check "$HERE/flat_traced.cpp"
echo "built: /tmp/sbm_flat_kernel_check"
