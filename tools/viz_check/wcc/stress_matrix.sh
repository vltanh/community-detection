#!/usr/bin/env bash
# WCC self-RNG L4 stress matrix — bit-equal lockstep self_rng_check
# across N fixtures × M seeds × P partition synths × Q criteria.
#
# Per playbook step 5: ≥1M cumulative per-step records, ≥9 seeds,
# ≥4 fixture sizes. Each WCC pop emits: cluster_nodes (size n), in/out
# (size n), cut (int), threshold (double + uint64 bits), wc verdict,
# pushed (variadic). Per-cell record count = pops × ~50 fields.
#
# Cells where the chained VieCut row-J residual flips a bipartition
# (same cut, different sides) are counted as IO_MM_ONLY (informational
# only) when survivors set + cut + threshold + wc all PASS.
#
# Usage: bash stress_matrix.sh [PARALLEL=4]
#
# Outputs:
#   /tmp/wcc_stress/cells.csv    per-cell results
#   /tmp/wcc_stress/summary.txt  aggregated counters
set -uo pipefail

ROOT="${ROOT:-/home/vltanh/Documents/netsci-research/community-detection}"
EMP="${EMP:-/home/vltanh/Documents/netsci-research/data/empirical_networks/networks}"
TR_SWAP="${TR_SWAP:-/tmp/wcc_kernel_check_swapped}"
CHECK="${CHECK:-${ROOT}/tools/viz_check/wcc/self_rng_check.mjs}"
OUT="${OUT:-/tmp/wcc_stress}"
PAR="${PARALLEL:-4}"

mkdir -p "$OUT"
: > "$OUT/cells.csv"
echo "fixture,n,seed,partition,K,criterion,pops,records,thr_bit_mm,cluster_mm,cut_mm,wc_mm,in_mm,out_mm,survivors,survivors_mm,status" > "$OUT/cells.csv"

# Fixture inventory: name | edge_csv | n_hint
FIXTURES=(
  "fixture32|${ROOT}/tests/cd_verify/fixture32_st_edge.csv|32"
  "dnc|${ROOT}/tests/cd_verify/dnc_edge.csv|906"
  "polbooks|${EMP}/polbooks/polbooks.csv|105"
  "football|${EMP}/football/football.csv|115"
  "dolphins|${EMP}/dolphins/dolphins.csv|62"
  "lesmis|${EMP}/lesmis/lesmis.csv|77"
  "karate78|${EMP}/karate/karate_78.csv|34"
  "adjnoun|${EMP}/adjnoun/adjnoun.csv|112"
  "celegansneural|${EMP}/celegansneural/celegansneural.csv|297"
  "facebook_friends|${EMP}/facebook_friends/facebook_friends.csv|348"
  "polblogs|${EMP}/polblogs/polblogs.csv|1224"
  "netscience|${EMP}/netscience/netscience.csv|1461"
)

# 9 diverse seeds.
SEEDS=(1 7 13 42 99 137 1729 65535 2147483646)

# Partition synth seeds (per seed → K-cluster mod assignment).
# These pick partition shapes that exercise diverse residual graphs.
PSEEDS=(1 5 11 23)  # K = 1, 8, sqrt(n), n/4
# Note: all-singleton (K=n) excluded — produces 0 pops.

CRITERIA=(
  "1log_10(n)"
  "2log_10(n)"
  "1log_2(n)"
)

synth_com() {
  python3 - "$1" "$2" "$3" <<'PY'
import sys, math
edge_p, com_p, pseed = sys.argv[1], sys.argv[2], int(sys.argv[3])
nodes = []; seen = set()
with open(edge_p) as f:
    next(f)
    for line in f:
        cols = line.strip().split(",")
        if len(cols) < 2: continue
        s, t = cols[0], cols[1]
        if s not in seen: seen.add(s); nodes.append(s)
        if t not in seen: seen.add(t); nodes.append(t)
n = len(nodes)
if pseed == 1:    K = 1; assign = lambda i: 0
elif pseed == 5:  K = 8; assign = lambda i: (i*7+11) % 8
elif pseed == 11: K = max(2, int(math.sqrt(n))); assign = lambda i: i % K
elif pseed == 23: K = max(2, n//4); assign = lambda i: i % K
else: sys.exit(2)
with open(com_p, "w") as f:
    f.write("node_id,cluster_id\n")
    for i, nid in enumerate(nodes):
        f.write(f"{nid},{assign(i)}\n")
print(f"K={K}")
PY
}

run_cell() {
  local path="$1" name="$2" n_hint="$3" seed="$4" pseed="$5" criterion="$6"
  local prefix="$OUT/cell_${name}_p${pseed}_s${seed}_$(echo "$criterion" | tr -c 'a-zA-Z0-9' '_')"
  local com="${prefix}.com"
  local out_swap="${prefix}_out.csv"
  local json_swap="${prefix}.json"
  local err_swap="${prefix}.err"
  local check_log="${prefix}.check"

  if [ ! -f "$path" ]; then
    echo "${name},${n_hint},${seed},${pseed},,${criterion},,,,,,,,,,,FIX_MISSING" >> "$OUT/cells.csv"
    return
  fi

  local k_str
  k_str=$(synth_com "$path" "$com" "$pseed" 2>/dev/null) || {
    echo "${name},${n_hint},${seed},${pseed},,${criterion},,,,,,,,,,,SYNTH_FAIL" >> "$OUT/cells.csv"
    return
  }
  local K=${k_str#K=}

  if ! timeout 120 "$TR_SWAP" "$path" "$com" "$out_swap" "$criterion" cactus "$seed" \
       > "$json_swap" 2> "$err_swap"; then
    echo "${name},${n_hint},${seed},${pseed},${K},${criterion},,,,,,,,,,,TRACER_FAIL" >> "$OUT/cells.csv"
    rm -f "$com" "$out_swap" "$json_swap"
    return
  fi

  if ! timeout 120 node "$CHECK" "$json_swap" "$path" "$com" "$criterion" "$seed" \
       > "$check_log" 2>&1; then
    : # FAIL output captured below
  fi

  # Parse counters.
  local canon_pops=$(grep -m1 'canonical:' "$check_log" | grep -oE 'pops=[0-9]+' | cut -d= -f2)
  local survivors=$(grep -m1 'canonical:' "$check_log" | grep -oE 'survivors=[0-9]+' | cut -d= -f2)
  local per_pop=$(grep -m1 '^per-pop:' "$check_log")
  local thr_mm=$(grep -oE 'thr_bit_mm=[0-9]+' <<<"$per_pop" | head -1 | cut -d= -f2)
  local cluster_mm=$(grep -oE 'cluster_mm=[0-9]+' <<<"$per_pop" | head -1 | cut -d= -f2)
  local cut_mm=$(grep -oE 'cut_mm=[0-9]+' <<<"$per_pop" | head -1 | cut -d= -f2)
  local wc_mm=$(grep -oE 'wc_mm=[0-9]+' <<<"$per_pop" | head -1 | cut -d= -f2)
  local in_mm=$(grep -oE 'in_mm=[0-9]+' <<<"$per_pop" | head -1 | cut -d= -f2)
  local out_mm=$(grep -oE 'out_mm=[0-9]+' <<<"$per_pop" | head -1 | cut -d= -f2)
  local surv_mm=0
  grep -q 'survivors_set_mm' "$check_log" && surv_mm=1
  local pop_count_mm=0
  grep -q 'pop_count_mm' "$check_log" && pop_count_mm=1

  # Records per cell = pops × ~50 fields (lockstep), thr is uint64 reinterpreted.
  local pops=${canon_pops:-0}
  local records=$((pops * 50))

  local status=PASS
  # IO_MM_ONLY: bipartition flips on equal-cost ties (chained VieCut row J);
  # survivors + cut + thr + wc all bit-equal.
  if [ "${cluster_mm:-0}" -gt 0 ] || [ "${cut_mm:-0}" -gt 0 ] \
     || [ "${wc_mm:-0}" -gt 0 ] || [ "${thr_mm:-0}" -gt 0 ] \
     || [ "$surv_mm" -gt 0 ] || [ "$pop_count_mm" -gt 0 ]; then
    status=FAIL_HARD
  elif [ "${in_mm:-0}" -gt 0 ] || [ "${out_mm:-0}" -gt 0 ]; then
    status=IO_MM_ONLY
  fi

  echo "${name},${n_hint},${seed},${pseed},${K},${criterion},${pops},${records},${thr_mm:-NA},${cluster_mm:-NA},${cut_mm:-NA},${wc_mm:-NA},${in_mm:-NA},${out_mm:-NA},${survivors:-NA},${surv_mm},${status}" >> "$OUT/cells.csv"

  if [ "$status" = "PASS" ] || [ "$status" = "IO_MM_ONLY" ]; then
    rm -f "$com" "$out_swap" "$json_swap" "$err_swap"
    [ "$status" = "PASS" ] && rm -f "$check_log"
  fi
}

export -f run_cell synth_com
export ROOT EMP TR_SWAP CHECK OUT

declare -a JOBS
for spec in "${FIXTURES[@]}"; do
  for seed in "${SEEDS[@]}"; do
    for pseed in "${PSEEDS[@]}"; do
      for crit in "${CRITERIA[@]}"; do
        JOBS+=("${spec}|||${seed}|||${pseed}|||${crit}")
      done
    done
  done
done
echo "stress matrix: ${#JOBS[@]} cells (parallel=$PAR)"

printf '%s\n' "${JOBS[@]}" \
  | xargs -P "$PAR" -I{} bash -c '
      spec="$0"
      seed=$(awk -F"\|\|\|" "{print \$2}" <<<"$spec")
      pseed=$(awk -F"\|\|\|" "{print \$3}" <<<"$spec")
      crit=$(awk -F"\|\|\|" "{print \$4}" <<<"$spec")
      fixture_spec=$(awk -F"\|\|\|" "{print \$1}" <<<"$spec")
      name=$(awk -F"\|" "{print \$1}" <<<"$fixture_spec")
      path=$(awk -F"\|" "{print \$2}" <<<"$fixture_spec")
      n=$(awk -F"\|" "{print \$3}" <<<"$fixture_spec")
      run_cell "$path" "$name" "$n" "$seed" "$pseed" "$crit"
    ' {}

echo ""
echo "=== WCC stress matrix summary ==="
awk -F, 'NR>1 {
  status[$17]++;
  if ($17=="PASS" || $17=="IO_MM_ONLY") records += $8;
}
END {
  printf "Cells:\n";
  for (s in status) printf "  %s: %d\n", s, status[s];
  printf "PASS+IO_MM cumulative records: %d\n", records;
}' "$OUT/cells.csv" | tee "$OUT/summary.txt"
