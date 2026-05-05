#!/usr/bin/env bash
# L0 driver: compile cpp leg, run both, diff first N raw uint32 outputs.
# Usage: ./run.sh [seed] [N]
set -euo pipefail
cd "$(dirname "$0")"

SEED="${1:-1}"
N="${2:-1000}"

echo "== Build cpp leg =="
g++ -O2 -std=c++17 -ffp-contract=off L0_rng_cpp.cpp -o L0_rng_cpp

CPP_OUT="$(mktemp)"
JS_OUT="$(mktemp)"
trap 'rm -f "$CPP_OUT" "$JS_OUT"' EXIT

echo "== Run cpp seed=$SEED N=$N =="
./L0_rng_cpp "$SEED" "$N" > "$CPP_OUT"
echo "== Run js seed=$SEED N=$N =="
node L0_rng_js.mjs "$SEED" "$N" > "$JS_OUT"

echo "== Diff =="
if diff -q "$CPP_OUT" "$JS_OUT" > /dev/null; then
  echo "PASS — first $N raw uint32 outputs bit-equal under seed=$SEED"
else
  echo "FAIL — divergence at:"
  diff "$CPP_OUT" "$JS_OUT" | head -40
  exit 1
fi
