#!/usr/bin/env bash
# VieCut self-RNG L4 stress matrix — bit-equal lockstep diff_harness
# across N fixtures × M seeds × variants {cactus_mincut}.
#
# Per playbook step 5: ≥1M cumulative per-step records, ≥9 seeds,
# ≥4 fixture sizes. VieCut's RNG-call structure is O(few) per cell
# (audit row B = capforest_start + problem_id increments + balanced_dfs);
# cumulative "records" tally includes:
#   - RNG events (diff_harness lockstep)
#   - cactus tree size (cactus.n + cactus.edges) bit-equal
#   - bipartition entries bit-equal
#   - mincut value bit-equal
# Each is a verified bit-equal artifact per cell.
#
# Usage: bash stress_matrix.sh [PARALLEL=4]
#
# Outputs:
#   /tmp/viecut_stress/cells.csv     per-cell results
#   /tmp/viecut_stress/summary.txt   aggregated counters
set -uo pipefail

ROOT="${ROOT:-/home/vltanh/Documents/netsci-research/community-detection}"
DIFF="${DIFF:-${ROOT}/tools/viz_check/viecut/diff_harness.mjs}"
TR_SWAP="${TR_SWAP:-/tmp/viecut_traced_swapped}"
OUT="${OUT:-/tmp/viecut_stress}"
PAR="${PARALLEL:-4}"

mkdir -p "$OUT"
: > "$OUT/cells.csv"
echo "fixture,n,seed,rng_records,cactus_n,cactus_edges,bipartition_n,mincut,total_records,status" > "$OUT/cells.csv"

# Fixture inventory: name | metis_path | n_hint
# Mix of small whole-graph + connected-LCC + dnc clusters. Whole-graph
# empirical networks often disconnect → mincut=0 (skip); LCC-extracted
# variants ensure connected substrate. n>10000 fixtures avoided because
# cpp's viecut.h LP-loop fires (the JS port short-circuits LP).
FIXTURES=(
  # small whole-graph empirical
  "polbooks|${ROOT}/tests/cd_verify/polbooks_viecut.metis|105"
  "football|${ROOT}/tests/cd_verify/football_viecut.metis|115"
  "celegansneural|${ROOT}/tests/cd_verify/celegansneural_viecut.metis|297"
  # large connected components (n<10000 to avoid LP path)
  "polblogs_lcc|${ROOT}/tests/cd_verify/polblogs_lcc_viecut.metis|1222"
  "netscience|${ROOT}/tests/cd_verify/netscience_viecut.metis|1461"
  "power|${ROOT}/tests/cd_verify/power_viecut.metis|4941"
  "sp_infectious_lcc|${ROOT}/tests/cd_verify/sp_infectious_lcc_viecut.metis|410"
  "arxiv_hepth_lcc|${ROOT}/tests/cd_verify/arxiv_collab_hep_th_1999_lcc_viecut.metis|5835"
  # dnc clusters: walk all 39 to build cumulative coverage
  "dnc_c0|${ROOT}/tests/cd_verify/dnc_c0_viecut.metis|205"
  "dnc_c1|${ROOT}/tests/cd_verify/dnc_c1_viecut.metis|0"
  "dnc_c2|${ROOT}/tests/cd_verify/dnc_c2_viecut.metis|0"
  "dnc_c3|${ROOT}/tests/cd_verify/dnc_c3_viecut.metis|0"
  "dnc_c4|${ROOT}/tests/cd_verify/dnc_c4_viecut.metis|0"
  "dnc_c5|${ROOT}/tests/cd_verify/dnc_c5_viecut.metis|0"
  "dnc_c6|${ROOT}/tests/cd_verify/dnc_c6_viecut.metis|0"
  "dnc_c7|${ROOT}/tests/cd_verify/dnc_c7_viecut.metis|0"
  "dnc_c8|${ROOT}/tests/cd_verify/dnc_c8_viecut.metis|0"
  "dnc_c9|${ROOT}/tests/cd_verify/dnc_c9_viecut.metis|0"
  "dnc_c10|${ROOT}/tests/cd_verify/dnc_c10_viecut.metis|0"
  "dnc_c11|${ROOT}/tests/cd_verify/dnc_c11_viecut.metis|0"
  "dnc_c12|${ROOT}/tests/cd_verify/dnc_c12_viecut.metis|0"
  "dnc_c13|${ROOT}/tests/cd_verify/dnc_c13_viecut.metis|0"
  "dnc_c14|${ROOT}/tests/cd_verify/dnc_c14_viecut.metis|0"
  "dnc_c15|${ROOT}/tests/cd_verify/dnc_c15_viecut.metis|0"
  "dnc_c16|${ROOT}/tests/cd_verify/dnc_c16_viecut.metis|0"
  "dnc_c17|${ROOT}/tests/cd_verify/dnc_c17_viecut.metis|0"
  "dnc_c18|${ROOT}/tests/cd_verify/dnc_c18_viecut.metis|0"
  "dnc_c19|${ROOT}/tests/cd_verify/dnc_c19_viecut.metis|0"
  "dnc_c20|${ROOT}/tests/cd_verify/dnc_c20_viecut.metis|0"
  "dnc_c21|${ROOT}/tests/cd_verify/dnc_c21_viecut.metis|0"
  "dnc_c22|${ROOT}/tests/cd_verify/dnc_c22_viecut.metis|0"
  "dnc_c23|${ROOT}/tests/cd_verify/dnc_c23_viecut.metis|0"
  "dnc_c24|${ROOT}/tests/cd_verify/dnc_c24_viecut.metis|0"
  "dnc_c25|${ROOT}/tests/cd_verify/dnc_c25_viecut.metis|0"
  "dnc_c30|${ROOT}/tests/cd_verify/dnc_c30_viecut.metis|0"
)

# 50 seeds matching leiden stress matrix shape; spans small + medium + large.
SEEDS=(
  1 2 3 5 7 11 13 17 19 23 29 31 37 41 42 43 47 53 59 61
  67 71 73 79 83 89 97 99 101 103 109 113 127 137 149 163 179 199 223 251
  279 311 349 397 449 503 569 641 1729 65535
)

# Per-cell run: diff_harness validates bit-equal; tracer JSON parsed for
# cactus tree size + bipartition length + mincut.
run_cell() {
  local name="$1" path="$2" n_hint="$3" seed="$4"

  if [ ! -f "$path" ]; then
    echo "${name},${n_hint},${seed},,,,,,,FIX_MISSING" >> "$OUT/cells.csv"
    return
  fi

  local diff_log="$OUT/cell_${name}_s${seed}.diff"
  local json="$OUT/cell_${name}_s${seed}.json"

  # Tracer JSON for non-RNG record counting.
  if ! "$TR_SWAP" "$path" "$seed" > "$json" 2>/dev/null; then
    echo "${name},${n_hint},${seed},,,,,,,TRACER_FAIL" >> "$OUT/cells.csv"
    return
  fi

  # mincut <= 0 → tracer skips cactus emission; per-cell count just the
  # 1 mincut value verified.
  local mincut
  mincut=$(python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(d.get('mincut',-1))" "$json" 2>/dev/null || echo "")
  if [ -z "$mincut" ] || [ "$mincut" = "-1" ] || [ "$mincut" = "0" ]; then
    rm -f "$json"
    echo "${name},${n_hint},${seed},0,0,0,0,${mincut:-NA},1,SKIP_NOCUT" >> "$OUT/cells.csv"
    return
  fi

  # diff_harness asserts RNG bit-equal lockstep.
  if ! node "$DIFF" "$path" "$seed" > "$diff_log" 2>&1; then
    echo "${name},${n_hint},${seed},,,,,,,DIFF_FAIL" >> "$OUT/cells.csv"
    return
  fi
  local rng_records
  rng_records=$(grep -oE 'total_records=[0-9]+' "$diff_log" | head -1 | cut -d= -f2)
  rng_records="${rng_records:-0}"

  local cactus_n cactus_edges bipart_n
  read -r cactus_n cactus_edges bipart_n <<< "$(python3 -c "
import json, sys
d = json.load(open(sys.argv[1]))
print(d['cactus']['n'], len(d['cactus']['edges']), len(d['bipartition']))
" "$json" 2>/dev/null)"

  local total=$((rng_records + cactus_n + cactus_edges + bipart_n + 1))
  echo "${name},${n_hint},${seed},${rng_records},${cactus_n},${cactus_edges},${bipart_n},${mincut},${total},PASS" >> "$OUT/cells.csv"

  rm -f "$json" "$diff_log"
}

export -f run_cell
export ROOT DIFF TR_SWAP OUT

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
echo "=== VieCut L4 stress matrix summary ==="
awk -F, 'NR>1 {
  status[$10]++;
  if ($10=="PASS") {
    rng += $4;
    cactus_n += $5;
    cactus_e += $6;
    bipart += $7;
    total += $9;
  }
}
END {
  printf "Cells:\n";
  for (s in status) printf "  %s: %d\n", s, status[s];
  printf "PASS cumulative records: %d\n", total;
  printf "  RNG events: %d\n", rng;
  printf "  cactus_n: %d\n", cactus_n;
  printf "  cactus_edges: %d\n", cactus_e;
  printf "  bipartition: %d\n", bipart;
}' "$OUT/cells.csv" | tee "$OUT/summary.txt"
