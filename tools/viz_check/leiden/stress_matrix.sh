#!/usr/bin/env bash
# Leiden self-RNG L4 stress matrix — top-level byte-equal across
# 50 seeds × ~21 fixtures (≤30K nodes) × {cpm@0.01, cpm@0.05, mod@1.0}.
#
# Top-level closure is the user-facing pedagogical contract per
# leiden_audit.md (inner-level admin algebra deferred = audit row M).
# Verifies bit-equal (v, fromComm, toComm, dQ, moved) tuple per visit
# between cpp libleidenalg standalone trace and JS production walker
# under matching MT19937 seed.
#
# Usage: bash stress_matrix.sh [PARALLEL=8]
#
# Outputs:
#   /tmp/leiden_stress/cells.csv     — per-cell results
#   /tmp/leiden_stress/summary.txt   — aggregated counters
set -uo pipefail

ROOT="${ROOT:-/home/vltanh/Documents/netsci-research/community-detection}"
EMP="${EMP:-/home/vltanh/Documents/netsci-research/data/empirical_networks/networks}"
TRACER="${TRACER:-/tmp/leiden_kernel_check}"
CHECK="${CHECK:-${ROOT}/tools/viz_check/leiden/self_rng_check.mjs}"
OUT="${OUT:-/tmp/leiden_stress}"
PAR="${PARALLEL:-8}"

cd "$ROOT" || exit 2
mkdir -p "$OUT"
: > "$OUT/cells.csv"
: > "$OUT/cell_logs"
echo "fixture,n,seed,quality,param,cpp_passes,cpp_visits,js_passes,js_visits,total_moves,v_mm,from_mm,to_mm,moved_mm,dQ_bit_mm,Q_canon,Q_js,status" > "$OUT/cells.csv"

# Fixture inventory: name | path | n
# (n approximate; validates via tracer output)
FIXTURES=(
  "fixture32|${ROOT}/tests/cd_verify/fixture32_st_edge.csv|32"
  "karate78|${EMP}/karate/karate_78.csv|34"
  "karate77|${EMP}/karate/karate_77.csv|34"
  "dolphins|${EMP}/dolphins/dolphins.csv|62"
  "lesmis|${EMP}/lesmis/lesmis.csv|77"
  "polbooks|${EMP}/polbooks/polbooks.csv|105"
  "football|${EMP}/football/football.csv|115"
  "adjnoun|${EMP}/adjnoun/adjnoun.csv|112"
  "celegansneural|${EMP}/celegansneural/celegansneural.csv|297"
  "facebook_friends|${EMP}/facebook_friends/facebook_friends.csv|348"
  "dnc|${ROOT}/tests/cd_verify/dnc_edge.csv|906"
  "euroroad|${EMP}/euroroad/euroroad.csv|1174"
  "polblogs|${EMP}/polblogs/polblogs.csv|1224"
  "netscience|${EMP}/netscience/netscience.csv|1461"
  "collins_yeast|${EMP}/collins_yeast/collins_yeast.csv|1622"
  "power|${EMP}/power/power.csv|4941"
  "arxiv_collab_hepth|${EMP}/arxiv_collab/arxiv_collab_hep-th-1999.csv|7610"
  "sp_infectious|${EMP}/sp_infectious/sp_infectious.csv|10972"
  "foldoc|${EMP}/foldoc/foldoc.csv|13356"
  "google|${EMP}/google/google.csv|15763"
  "cora|${EMP}/cora/cora.csv|23166"
)

# 50 seeds spanning small + medium + large ints; avoid 0 (igraph remaps to 4357).
SEEDS=(
  1 2 3 5 7 11 13 17 19 23 29 31 37 41 42 43 47 53 59 61
  67 71 73 79 83 89 97 99 101 103 109 113 127 137 149 163 179 199 223 251
  279 311 349 397 449 503 569 641 1729 65535
)
QUALITIES=(
  "cpm 0.01"
  "cpm 0.05"
  "mod 1.0"
)

run_cell() {
  local fix_path="$1" name="$2" n_hint="$3" seed="$4" qspec="$5"
  local quality param
  read -r quality param <<<"$qspec"
  local prefix="$OUT/cell_${name}_${quality}_${param}_${seed}"
  local trace_full="${prefix}.full"
  local trace="${prefix}.json"
  local stderr_log="${prefix}.err"
  local check_log="${prefix}.check"

  if [ ! -f "$fix_path" ]; then
    echo "${name},${n_hint},${seed},${quality},${param},,,,,,,,,,,,,FIX_MISSING" >> "$OUT/cells.csv"
    return
  fi

  # Run tracer; [TRACE-LD] prints already route to stderr, so JSON is
  # stdout-clean. Don't use `| grep -v` — GNU grep truncates JSON lines
  # >~64K (arxiv-scale fixtures emit single-line moves arrays >700K).
  if ! "$TRACER" "$fix_path" "${prefix}.com" "$quality" "$param" "$seed" 1 \
       > "$trace" 2>"$stderr_log"; then
    echo "${name},${n_hint},${seed},${quality},${param},,,,,,,,,,,,,TRACER_FAIL" >> "$OUT/cells.csv"
    rm -f "$trace" "${prefix}.com"
    return
  fi

  # Run JS self-RNG check (top-level only, maxOuterLevels=1).
  if ! timeout 30 node "$CHECK" "$trace" "$fix_path" "$quality" "$param" "$seed" \
       > "$check_log" 2>>"$stderr_log"; then
    # PASS-fail status decided by parser below; only log the timeout/crash.
    if grep -q "FAIL: self-RNG" "$check_log"; then
      :
    elif ! grep -q "JS production walker bit-equal" "$check_log"; then
      echo "${name},${n_hint},${seed},${quality},${param},,,,,,,,,,,,,CHECK_CRASH" >> "$OUT/cells.csv"
      return
    fi
  fi

  # Parse counters out of the check log.
  local cpp_line js_line per_v_line q_line status_line
  cpp_line=$(grep -m1 '^canonical:' "$check_log")
  js_line=$(grep -m1 '^js:' "$check_log")
  per_v_line=$(grep -m1 '^per-visit:' "$check_log")
  q_line=$(grep -m1 '^Q_canon=' "$check_log")
  status_line=$(grep -m1 -E '^(PASS|FAIL):' "$check_log")

  local cpp_passes cpp_visits js_passes js_visits
  cpp_passes=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^passes=/) {split($i,a,"="); print a[2]; exit}}' <<<"$cpp_line")
  cpp_visits=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^visits=/) {split($i,a,"="); print a[2]; exit}}' <<<"$cpp_line")
  js_passes=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^passes=/) {split($i,a,"="); print a[2]; exit}}' <<<"$js_line")
  js_visits=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^visits=/) {split($i,a,"="); print a[2]; exit}}' <<<"$js_line")

  local v_mm from_mm to_mm moved_mm dq_mm total_moves
  v_mm=$(grep -oE 'v_mm=[0-9]+' <<<"$per_v_line" | head -1 | cut -d= -f2)
  from_mm=$(grep -oE 'from_mm=[0-9]+' <<<"$per_v_line" | head -1 | cut -d= -f2)
  to_mm=$(grep -oE 'to_mm=[0-9]+' <<<"$per_v_line" | head -1 | cut -d= -f2)
  moved_mm=$(grep -oE 'moved_mm=[0-9]+' <<<"$per_v_line" | head -1 | cut -d= -f2)
  dq_mm_total=$(grep -oE 'dQ_bit_mm=[0-9]+/[0-9]+' <<<"$per_v_line" | head -1 | cut -d= -f2)
  dq_mm="${dq_mm_total%%/*}"
  total_moves="${dq_mm_total##*/}"

  local q_canon q_js
  q_canon=$(grep -oE 'Q_canon=[-0-9.eE+]+' <<<"$q_line" | head -1 | cut -d= -f2)
  q_js=$(grep -oE 'Q_js=[-0-9.eE+]+' <<<"$q_line" | head -1 | cut -d= -f2)

  local status
  status=$(awk -F: '{print $1}' <<<"$status_line")

  echo "${name},${n_hint},${seed},${quality},${param},${cpp_passes:-NA},${cpp_visits:-NA},${js_passes:-NA},${js_visits:-NA},${total_moves:-NA},${v_mm:-NA},${from_mm:-NA},${to_mm:-NA},${moved_mm:-NA},${dq_mm:-NA},${q_canon:-NA},${q_js:-NA},${status:-NA}" >> "$OUT/cells.csv"

  # Clean up trace files to save disk; keep stderr_log + check_log for FAIL diag.
  if [ "$status" = "PASS" ]; then
    rm -f "$trace" "${prefix}.com" "$stderr_log" "$check_log"
  fi
}

export -f run_cell
export ROOT EMP TRACER CHECK OUT

total=0
declare -a JOBS
for spec in "${FIXTURES[@]}"; do
  for seed in "${SEEDS[@]}"; do
    for q in "${QUALITIES[@]}"; do
      JOBS+=("${spec}|||${seed}|||${q}")
      total=$((total+1))
    done
  done
done
echo "stress matrix: $total cells (parallel=$PAR)"

# Run via xargs -P
printf '%s\n' "${JOBS[@]}" \
  | xargs -P "$PAR" -I{} bash -c '
      spec="$0"
      seed=$(awk -F"\|\|\|" "{print \$2}" <<<"$spec")
      q=$(awk -F"\|\|\|" "{print \$3}" <<<"$spec")
      fixture_spec=$(awk -F"\|\|\|" "{print \$1}" <<<"$spec")
      name=$(awk -F"\|" "{print \$1}" <<<"$fixture_spec")
      path=$(awk -F"\|" "{print \$2}" <<<"$fixture_spec")
      n=$(awk -F"\|" "{print \$3}" <<<"$fixture_spec")
      run_cell "$path" "$name" "$n" "$seed" "$q"
    ' {}

# Summary
echo ""
echo "=== Stress matrix summary ==="
awk -F, 'NR>1 {
  status[$18]++;
  if ($18=="PASS") {
    visits += $9;
    moves += $10;
    bit_mm += $15;
  }
}
END {
  printf "Cells:\n";
  for (s in status) printf "  %s: %d\n", s, status[s];
  printf "PASS cumulative top-level visits: %d\n", visits;
  printf "PASS cumulative moves: %d\n", moves;
  printf "PASS cumulative dQ bit-mismatches: %d / %d\n", bit_mm, moves;
}' "$OUT/cells.csv" | tee "$OUT/summary.txt"
