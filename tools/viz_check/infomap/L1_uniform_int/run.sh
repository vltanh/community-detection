#!/usr/bin/env bash
# L1 driver: build cpp leg, run both, diff CSV rows.
set -euo pipefail
cd "$(dirname "$0")"

SEED="${1:-1}"
N="${2:-100000}"

g++ -O2 -std=c++17 -ffp-contract=off L1_unifint_cpp.cpp -o L1_unifint_cpp

CPP_OUT="$(mktemp)"; JS_OUT="$(mktemp)"
trap 'rm -f "$CPP_OUT" "$JS_OUT"' EXIT

./L1_unifint_cpp "$SEED" "$N" > "$CPP_OUT"
node L1_unifint_js.mjs "$SEED" "$N" > "$JS_OUT"

if diff -q "$CPP_OUT" "$JS_OUT" > /dev/null; then
  echo "PASS — $N draws bit-equal under seed=$SEED (uniform_int_distribution<unsigned int> mirror)"
else
  echo "FAIL — first 20 divergent lines:"
  diff "$CPP_OUT" "$JS_OUT" | head -40
  exit 1
fi
