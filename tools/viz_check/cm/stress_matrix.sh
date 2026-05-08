#!/usr/bin/env bash
# CM L4 self-RNG end-to-end stress matrix — bit-equal lockstep
# self_rng_check.mjs across N (graph,com) pairs × M seeds × cpm@0.0001.
#
# Per playbook step 5: ≥1M cumulative per-step records, ≥9 seeds, ≥4 fixtures.
# Per CM audit: chains VieCut (L4 PASS 1700/1700) + Leiden (L4 PASS 3313/3313)
# + WCC threshold (jsLog port). Avoid seed=0 (igraph MT19937 quirk maps 0→4357).
#
# Usage: bash stress_matrix.sh [PARALLEL=4]
#
# Outputs:
#   /tmp/cm_stress/cells.csv     per-cell results
#   /tmp/cm_stress/summary.txt   aggregated counters
set -uo pipefail

ROOT="${ROOT:-/home/vltanh/Documents/netsci-research/community-detection}"
EMP="${EMP:-/home/vltanh/Documents/netsci-research/data/empirical_networks/networks}"
TR_SWAP="${TR_SWAP:-/tmp/cm_kernel_check_swapped}"
CHECK="${CHECK:-${ROOT}/tools/viz_check/cm/self_rng_check.mjs}"
OUT="${OUT:-/tmp/cm_stress}"
PAR="${PARALLEL:-4}"

cd "$ROOT" || exit 2
mkdir -p "$OUT"
: > "$OUT/cells.csv"
echo "fixture,n,seed,resolution,cpp_pops,js_pops,cpp_survivors,js_survivors,total_records,status,first_div" > "$OUT/cells.csv"

# (edge.csv, com.csv) fixture pairs. Synthesise com.csv per (fixture, seed)
# slot via mod-K assignment so each cell exercises a distinct membership.
FIXTURES=(
  "fixture32|${ROOT}/tests/cd_verify/fixture32_st_edge.csv|32"
  "dnc|${ROOT}/tests/cd_verify/dnc_edge.csv|906"
  "polbooks|${EMP}/polbooks/polbooks.csv|105"
  "football|${EMP}/football/football.csv|115"
  "celegansneural|${EMP}/celegansneural/celegansneural.csv|297"
  "facebook_friends|${EMP}/facebook_friends/facebook_friends.csv|348"
)

# Avoid seed=0 (igraph MT19937 quirk per Leiden audit).
SEEDS=(1 7 13 42 99 137 1729 65535 2147483646)
RESOLUTION=0.0001
CRITERION="1log_10(n)"

synth_com() {
  # synth_com edge_csv com_csv [K] (default K=4)
  local edge_p="$1" com_p="$2" K="${3:-4}"
  python3 - "$edge_p" "$com_p" "$K" <<'PY'
import sys
edge_p, com_p, K = sys.argv[1], sys.argv[2], int(sys.argv[3])
nodes = []
seen = set()
with open(edge_p) as f:
    next(f)  # header
    for line in f:
        parts = line.strip().split(",")[:2]
        if len(parts) < 2: continue
        s, t = parts
        if s not in seen: seen.add(s); nodes.append(s)
        if t not in seen: seen.add(t); nodes.append(t)
with open(com_p, "w") as f:
    f.write("node_id,cluster_id\n")
    for i, nid in enumerate(nodes):
        f.write(f"{nid},{i % K}\n")
PY
}

run_cell() {
  local name="$1" path="$2" n_hint="$3" seed="$4"
  local prefix="$OUT/cell_${name}_s${seed}"
  local com="${prefix}.com"
  local out_csv="${prefix}.out"
  local trace_json="${prefix}.cpp.json"
  local check_log="${prefix}.check"

  if [ ! -f "$path" ]; then
    echo "${name},${n_hint},${seed},${RESOLUTION},,,,,,FIX_MISSING," >> "$OUT/cells.csv"
    return
  fi

  # K varies by seed → diverse membership inputs.
  local K=$(( (seed % 7) + 2 ))  # K ∈ {2..8}
  if ! synth_com "$path" "$com" "$K" 2>/dev/null; then
    echo "${name},${n_hint},${seed},${RESOLUTION},,,,,,SYNTH_FAIL," >> "$OUT/cells.csv"
    return
  fi

  # Run cm tracer (TRACER_MODE swapped build).
  if ! timeout 300 "$TR_SWAP" "$path" "$com" "$out_csv" "$CRITERION" "$RESOLUTION" "$seed" \
       > "$trace_json" 2>/dev/null; then
    echo "${name},${n_hint},${seed},${RESOLUTION},,,,,,TRACER_FAIL," >> "$OUT/cells.csv"
    rm -f "$com" "$out_csv"
    return
  fi

  # Run JS replay (self_rng_check.mjs; no oracles).
  if ! timeout 300 node "$CHECK" "$trace_json" "$path" "$com" \
       "$CRITERION" "$RESOLUTION" "$seed" > "$check_log" 2>&1; then
    # Parse first-divergence line; CHECK_FAIL is principled, distinguish from CHECK_CRASH.
    local first_div=""
    if grep -q "FIRST DIVERGENCE" "$check_log"; then
      first_div=$(grep -m1 "FIRST DIVERGENCE" "$check_log" | sed 's/[",]/_/g' | head -c 80)
      local cpp_pops js_pops cpp_surv js_surv
      cpp_pops=$(grep -oE 'cpp pops=[0-9]+' "$check_log" | head -1 | cut -d= -f2)
      js_pops=$(grep -oE 'js pops=[0-9]+' "$check_log" | head -1 | cut -d= -f2)
      cpp_surv=$(grep -oE 'cpp=[0-9]+ js=' "$check_log" | head -1 | grep -oE 'cpp=[0-9]+' | cut -d= -f2)
      js_surv=$(grep -oE 'js=[0-9]+' "$check_log" | tail -1 | cut -d= -f2)
      echo "${name},${n_hint},${seed},${RESOLUTION},${cpp_pops:-NA},${js_pops:-NA},${cpp_surv:-NA},${js_surv:-NA},0,DIFF_FAIL,${first_div}" >> "$OUT/cells.csv"
    else
      echo "${name},${n_hint},${seed},${RESOLUTION},,,,,,CHECK_CRASH," >> "$OUT/cells.csv"
    fi
    return
  fi

  # PASS: parse counters.
  local cpp_pops js_pops surv_count
  cpp_pops=$(grep -oE 'cpp pops=[0-9]+' "$check_log" | head -1 | cut -d= -f2)
  js_pops=$(grep -oE 'js pops=[0-9]+' "$check_log" | head -1 | cut -d= -f2)
  surv_count=$(grep -oE '[0-9]+ clusters w/ lineage' "$check_log" | head -1 | grep -oE '^[0-9]+' || echo 0)
  local total_records=$(( ${cpp_pops:-0} + ${surv_count:-0} ))
  echo "${name},${n_hint},${seed},${RESOLUTION},${cpp_pops:-0},${js_pops:-0},${surv_count:-0},${surv_count:-0},${total_records},PASS," >> "$OUT/cells.csv"

  # Cleanup on PASS.
  rm -f "$com" "$out_csv" "$trace_json" "$check_log"
}

export -f run_cell synth_com
export ROOT EMP TR_SWAP CHECK OUT RESOLUTION CRITERION

declare -a JOBS
for spec in "${FIXTURES[@]}"; do
  for seed in "${SEEDS[@]}"; do
    JOBS+=("${spec}|||${seed}")
  done
done
echo "stress matrix: ${#JOBS[@]} cells (parallel=$PAR)"

printf '%s\n' "${JOBS[@]}" \
  | xargs -P "$PAR" -I{} bash -c '
      spec="$0"
      seed=$(awk -F"\|\|\|" "{print \$2}" <<<"$spec")
      fix_spec=$(awk -F"\|\|\|" "{print \$1}" <<<"$spec")
      name=$(awk -F"\|" "{print \$1}" <<<"$fix_spec")
      path=$(awk -F"\|" "{print \$2}" <<<"$fix_spec")
      n=$(awk -F"\|" "{print \$3}" <<<"$fix_spec")
      run_cell "$name" "$path" "$n" "$seed"
    ' {}

echo ""
echo "=== CM L4 stress matrix summary ==="
awk -F, 'NR>1 {
  status[$10]++;
  if ($10=="PASS") records += $9;
}
END {
  printf "Cells:\n";
  for (s in status) printf "  %s: %d\n", s, status[s];
  printf "PASS cumulative records: %d\n", records;
}' "$OUT/cells.csv" | tee "$OUT/summary.txt"
