#!/usr/bin/env bash
# L2 alt: cpp jsLog2 (fdlibm jsLog * Math.LOG2E + log2(1)=0 special) vs JS
# jsLog2 (Math.log * Math.LOG2E + log2(1)=0 special). Expect bit-equal across all
# inputs since both sides compose the same fdlibm log + the same constant.
set -euo pipefail
cd "$(dirname "$0")"

N="${1:-100000}"
SEED="${2:-7}"

g++ -O2 -std=c++17 -ffp-contract=off -fno-fast-math L2_jsLog2_cpp.cpp -o L2_jsLog2_cpp

CPP_OUT="$(mktemp)"; JS_OUT="$(mktemp)"
trap 'rm -f "$CPP_OUT" "$JS_OUT"' EXIT

./L2_jsLog2_cpp "$N" "$SEED" > "$CPP_OUT"
node L2_jsLog2_js.mjs < "$CPP_OUT" > "$JS_OUT"

ROWS=$(($(wc -l < "$CPP_OUT") - 1))
MIS=$(paste -d',' <(tail -n +2 "$CPP_OUT" | cut -d',' -f2) <(tail -n +2 "$JS_OUT" | cut -d',' -f2) \
       | awk -F',' '$1 != $2' | wc -l)

echo "rows=$ROWS  mismatches=$MIS"
if [ "$MIS" -eq 0 ]; then
  echo "PASS — jsLog2(cpp) == jsLog2(JS) bit-equal across $ROWS inputs (seed=$SEED)"
else
  echo "FAIL — first 5 mismatches:"
  paste -d',' <(tail -n +2 "$CPP_OUT") <(tail -n +2 "$JS_OUT" | cut -d',' -f2) \
    | awk -F',' '$2 != $3' | head -5
  exit 1
fi
