#!/usr/bin/env bash
# L2 driver: build cpp leg, dump xbits + cpp_log2bits, compute JS Math.log2 on
# the same xbits, diff bit columns. Reports total mismatches + first 10 examples.
set -euo pipefail
cd "$(dirname "$0")"

N="${1:-100000}"
SEED="${2:-7}"

g++ -O2 -std=c++17 -ffp-contract=off -fno-fast-math L2_log2_cpp.cpp -o L2_log2_cpp

CPP_OUT="$(mktemp)"; JS_OUT="$(mktemp)"
trap 'rm -f "$CPP_OUT" "$JS_OUT"' EXIT

./L2_log2_cpp "$N" "$SEED" > "$CPP_OUT"
node L2_log2_js.mjs < "$CPP_OUT" > "$JS_OUT"

# Both files have rows xbits,logbits. cpp's logbits = std::log2 bits, js's = Math.log2 bits.
# Use awk to count mismatches.
ROWS=$(($(wc -l < "$CPP_OUT") - 1))
MIS=$(paste -d',' <(tail -n +2 "$CPP_OUT" | cut -d',' -f2) <(tail -n +2 "$JS_OUT" | cut -d',' -f2) \
       | awk -F',' '$1 != $2' | wc -l)

echo "rows=$ROWS  mismatches=$MIS"
if [ "$MIS" -eq 0 ]; then
  echo "PASS — std::log2 == Math.log2 bit-equal across $ROWS inputs (seed=$SEED)"
else
  echo "FAIL — first 10 mismatches:"
  paste -d',' <(tail -n +2 "$CPP_OUT") <(tail -n +2 "$JS_OUT" | cut -d',' -f2) \
    | awk -F',' '$2 != $3' | head -10
  exit 1
fi
