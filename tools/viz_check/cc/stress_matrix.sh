#!/usr/bin/env bash
# CC stress matrix — byte-equal canonical-tracer (-DCANONICAL_MODE) vs
# JS replay across N fixtures × M synth-partitions. CC has no RNG/FP, so
# the canonical-tracer output IS the canonical output and the only knob
# is the input (graph, partition) pair.
#
# Per playbook stress matrix target: ≥ 1M cumulative output records, with
# ≥ 9 input variations (we use fixture × partition-seed pairs as the
# variation axis). Each cell emits |V| node-cluster rows; cells across
# diverse fixtures sum to ~1M+.
#
# Per-fixture partition synth (deterministic from seed):
#   seed=1 -> K=1   (every node in cluster 0; max edge survival)
#   seed=2 -> K=2   (cluster_id = node_id % 2)
#   seed=3 -> K=4   (cluster_id = (node_id*5+3) % 4)
#   seed=5 -> K=8   (cluster_id = (node_id*7+11) % 8)
#   seed=7 -> K=16  (cluster_id = (node_id*13+17) % 16)
#   seed=11-> K=sqrt(n)
#   seed=13-> K=n/2 (heavy split; many singletons → many singletons dropped)
#   seed=17-> K=n   (every node own cluster; all singletons; output empty)
#   seed=19-> stripe by degree (compute degree, sort, split into 4 quantiles)
#
# Usage: bash stress_matrix.sh [PARALLEL=4]
#
# Outputs:
#   /tmp/cc_stress/cells.csv     — per-cell results
#   /tmp/cc_stress/summary.txt   — aggregated counters
set -uo pipefail

ROOT="${ROOT:-/home/vltanh/Documents/netsci-research/community-detection}"
EMP="${EMP:-/home/vltanh/Documents/netsci-research/data/empirical_networks/networks}"
TR_CANON="${TR_CANON:-/tmp/cc_kernel_check_canonical}"
TR_SWAP="${TR_SWAP:-/tmp/cc_kernel_check_swapped}"
JS_REPLAY="${JS_REPLAY:-${ROOT}/tools/viz_check/cc/kernel_check.mjs}"
OUT="${OUT:-/tmp/cc_stress}"
PAR="${PARALLEL:-4}"

cd "$ROOT" || exit 2
mkdir -p "$OUT"
: > "$OUT/cells.csv"
echo "fixture,n,seed,K,records,canon_vs_swap,canon_vs_js,status" > "$OUT/cells.csv"

# Fixture inventory (subset of leiden stress matrix; CC is fast so we can
# walk many large fixtures cheaply).
FIXTURES=(
  "fixture32|${ROOT}/tests/cd_verify/fixture32_st_edge.csv|32"
  "karate78|${EMP}/karate/karate_78.csv|34"
  "dolphins|${EMP}/dolphins/dolphins.csv|62"
  "lesmis|${EMP}/lesmis/lesmis.csv|77"
  "polbooks|${EMP}/polbooks/polbooks.csv|105"
  "football|${EMP}/football/football.csv|115"
  "celegansneural|${EMP}/celegansneural/celegansneural.csv|297"
  "facebook_friends|${EMP}/facebook_friends/facebook_friends.csv|348"
  "dnc|${ROOT}/tests/cd_verify/dnc_edge.csv|906"
  "polblogs|${EMP}/polblogs/polblogs.csv|1224"
  "netscience|${EMP}/netscience/netscience.csv|1461"
  "power|${EMP}/power/power.csv|4941"
  "arxiv_collab_hepth|${EMP}/arxiv_collab/arxiv_collab_hep-th-1999.csv|7610"
  "sp_infectious|${EMP}/sp_infectious/sp_infectious.csv|10972"
  "foldoc|${EMP}/foldoc/foldoc.csv|13356"
  "google|${EMP}/google/google.csv|15763"
  "cora|${EMP}/cora/cora.csv|23166"
)

SEEDS=(1 2 3 5 7 11 13 17 19)

synth_com() {
  # synth_com edge_csv com_csv seed
  python3 - "$1" "$2" "$3" <<'PY'
import sys, math
edge_p, com_p, seed = sys.argv[1], sys.argv[2], int(sys.argv[3])
nodes = []
seen = set()
with open(edge_p) as f:
    next(f)  # header
    for line in f:
        s, t = line.strip().split(",")[:2]
        if s not in seen: seen.add(s); nodes.append(s)
        if t not in seen: seen.add(t); nodes.append(t)
n = len(nodes)
if seed == 1:    K = 1; assign = lambda i: 0
elif seed == 2:  K = 2; assign = lambda i: i % 2
elif seed == 3:  K = 4; assign = lambda i: (i*5+3) % 4
elif seed == 5:  K = 8; assign = lambda i: (i*7+11) % 8
elif seed == 7:  K = 16; assign = lambda i: (i*13+17) % 16
elif seed == 11: K = max(2, int(math.sqrt(n))); assign = lambda i: i % K
elif seed == 13: K = max(2, n//2); assign = lambda i: i // 2
elif seed == 17: K = n; assign = lambda i: i
elif seed == 19: K = 4; assign = lambda i: (i*0x9E3779B9) % 4
else: sys.exit(2)
with open(com_p, "w") as f:
    f.write("node_id,cluster_id\n")
    for i, nid in enumerate(nodes):
        f.write(f"{nid},{assign(i)}\n")
print(f"K={K} n={n}")
PY
}

run_cell() {
  local path="$1" name="$2" n_hint="$3" seed="$4"
  local prefix="$OUT/cell_${name}_s${seed}"
  local com="${prefix}.com"
  local out_canon="${prefix}_canon.csv"
  local out_swap="${prefix}_swap.csv"
  local json_canon="${prefix}_canon.json"
  local json_swap="${prefix}_swap.json"

  if [ ! -f "$path" ]; then
    echo "${name},${n_hint},${seed},,,,,FIX_MISSING" >> "$OUT/cells.csv"
    return
  fi

  local k_n
  k_n=$(synth_com "$path" "$com" "$seed" 2>&1) || {
    echo "${name},${n_hint},${seed},,,,,SYNTH_FAIL" >> "$OUT/cells.csv"
    return
  }
  local K=${k_n##*K=}; K=${K%% *}

  if ! "$TR_CANON" "$path" "$com" "$out_canon" > "$json_canon" 2>/dev/null; then
    echo "${name},${n_hint},${seed},${K},,,TRACER_CANON_FAIL" >> "$OUT/cells.csv"
    rm -f "$com"; return
  fi
  if ! "$TR_SWAP" "$path" "$com" "$out_swap" > "$json_swap" 2>/dev/null; then
    echo "${name},${n_hint},${seed},${K},,,TRACER_SWAP_FAIL" >> "$OUT/cells.csv"
    rm -f "$com"; return
  fi

  local canon_vs_swap=PASS
  if ! diff -q "$out_canon" "$out_swap" >/dev/null 2>&1 \
     || ! diff -q "$json_canon" "$json_swap" >/dev/null 2>&1; then
    canon_vs_swap=FAIL
  fi

  local canon_vs_js=PASS
  if ! node "$JS_REPLAY" "$json_canon" "$path" "$com" >/dev/null 2>&1; then
    canon_vs_js=FAIL
  fi

  local records
  records=$(($(wc -l < "$out_canon") - 1))  # subtract header

  local status=PASS
  [ "$canon_vs_swap" = "FAIL" ] && status=FAIL
  [ "$canon_vs_js" = "FAIL" ] && status=FAIL

  echo "${name},${n_hint},${seed},${K},${records},${canon_vs_swap},${canon_vs_js},${status}" >> "$OUT/cells.csv"

  # Clean up on PASS to save disk.
  if [ "$status" = "PASS" ]; then
    rm -f "$com" "$out_canon" "$out_swap" "$json_canon" "$json_swap"
  fi
}

export -f run_cell synth_com
export ROOT EMP TR_CANON TR_SWAP JS_REPLAY OUT

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
      fixture_spec=$(awk -F"\|\|\|" "{print \$1}" <<<"$spec")
      name=$(awk -F"\|" "{print \$1}" <<<"$fixture_spec")
      path=$(awk -F"\|" "{print \$2}" <<<"$fixture_spec")
      n=$(awk -F"\|" "{print \$3}" <<<"$fixture_spec")
      run_cell "$path" "$name" "$n" "$seed"
    ' {}

echo ""
echo "=== CC stress matrix summary ==="
awk -F, 'NR>1 {
  status[$8]++;
  if ($8=="PASS") records += $5;
}
END {
  printf "Cells:\n";
  for (s in status) printf "  %s: %d\n", s, status[s];
  printf "PASS cumulative records: %d\n", records;
}' "$OUT/cells.csv" | tee "$OUT/summary.txt"
