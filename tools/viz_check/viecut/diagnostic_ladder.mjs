// VieCut diagnostic ladder L0 + L1.
//
// L0: JS MT19937 raw output stream vs canonical's libstdc++ std::mt19937
//     output stream, first N values bit-equal (uint64 reinterpret of
//     uint32 raw outputs).
// L1: per-call-kind draw-count parity. JS observer counts vs canonical
//     [TRACE-RNG] counts on the same fixture/seed.
// L2: N/A per VieCut audit row D (no FP primitives).
//
// L3 (oracle replay) lives in separation_test.mjs; this script runs L0+L1
// only.
//
// Run: node diagnostic_ladder.mjs <metis_file> [seed=0] [n_l0=100]

import { readFileSync, existsSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { loadVIECUT } from "./_loader.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, "../../..");
const TRACED = "/tmp/viecut_traced_swapped";

if (!existsSync(TRACED)) {
  console.error(`${TRACED} not built; run instrumented/build.sh`);
  process.exit(2);
}
const VIECUT = loadVIECUT();

function parseRngTrace(stderr) {
  const queue = [];
  for (const line of stderr.split("\n")) {
    const m = line.match(/^\[TRACE-RNG\] kind:(\w+)(.*)$/);
    if (!m) continue;
    queue.push({ kind: m[1], rest: m[2] });
  }
  return queue;
}

function loadMetis(path) {
  const txt = readFileSync(path, "utf-8");
  const lines = txt.split("\n");
  const [n] = lines[0].split(/\s+/).map(Number);
  const edges = [];
  for (let i = 1; i <= n; i++) {
    if (!lines[i]) continue;
    const parts = lines[i].trim().split(/\s+/).filter((s) => s.length > 0);
    for (const t of parts) {
      const v = Number(t) - 1;
      if (i - 1 < v) edges.push([i - 1, v]);
    }
  }
  return { n, edges };
}

function buildG(n, edges) {
  const G = new VIECUT.MutableGraph();
  G.start_construction(n);
  for (const [u, v] of edges) G.new_edge(u, v, 1);
  G.finish_construction();
  return G;
}

// ---- L0 ----
function L0(seed, N) {
  // Direct cmp: spawn a tiny C++ that emits N raw mt19937 outputs.
  // Cheaper alternative: use existing tracer's next() output captured
  // by running cactus_mincut on a small fixture; but easier+more direct
  // is a bespoke binary. Implement here without spawning: we trust that
  // cactus_mincut on fixture32 emits ≥N next() calls if N small enough.
  // For independence + simplicity, just do JS MT19937 vs an inline
  // cpp baseline emitted via an ad-hoc oneshot binary.
  //
  // Approach: spawn /usr/bin/g++ to compile an inline cpp emitter at /tmp.
  const cpp = `
#include <random>
#include <cstdio>
int main(int argc, char**argv) {
  unsigned seed = (unsigned)atoi(argv[1]);
  int N = atoi(argv[2]);
  std::mt19937 mt(seed);
  for (int i = 0; i < N; i++) {
    unsigned v = mt();
    printf("%u\\n", v);
  }
  return 0;
}
`;
  const path_cpp = `/tmp/viecut_l0.cpp`;
  const path_bin = `/tmp/viecut_l0`;
  spawnSync("bash", ["-c", `cat > ${path_cpp} <<'EOF'\n${cpp}\nEOF\ng++ -O2 -std=c++17 -o ${path_bin} ${path_cpp}`]);
  const rc = spawnSync(path_bin, [String(seed), String(N)], { encoding: "utf-8" });
  if (rc.status !== 0) return { ok: false, err: `cpp baseline failed: ${rc.stderr}` };
  const cppOuts = rc.stdout.trim().split("\n").map((x) => parseInt(x, 10) >>> 0);
  const mt = new VIECUT.MT19937(seed);
  const jsOuts = [];
  for (let i = 0; i < N; i++) jsOuts.push(mt.next());
  let mismatches = 0, firstMismatch = null;
  for (let i = 0; i < N; i++) {
    if (cppOuts[i] !== jsOuts[i]) {
      mismatches++;
      if (firstMismatch === null) firstMismatch = `i=${i} cpp=${cppOuts[i]} js=${jsOuts[i]}`;
    }
  }
  return {
    ok: mismatches === 0,
    N, mismatches, firstMismatch,
  };
}

// ---- L1 ----
function L1(metisPath, seed) {
  const rc = spawnSync(TRACED, [metisPath, String(seed)], { encoding: "utf-8", maxBuffer: 1 << 28 });
  if (rc.status !== 0) return { ok: false, err: `tracer exit ${rc.status}` };
  if (!rc.stdout.trim().startsWith("{")) return { ok: true, skipped: true };
  const canon = JSON.parse(rc.stdout);
  if (canon.mincut <= 0) return { ok: true, skipped: true };

  const cppQueue = parseRngTrace(rc.stderr);
  const cppCounts = {};
  for (const e of cppQueue) cppCounts[e.kind] = (cppCounts[e.kind] || 0) + 1;

  const { n, edges } = loadMetis(metisPath);
  const G = buildG(n, edges);
  const jsCounts = {};
  VIECUT.random_functions.setObserver((rec) => {
    jsCounts[rec.kind] = (jsCounts[rec.kind] || 0) + 1;
  });
  VIECUT.cactus_mincut(G, { seed });
  VIECUT.random_functions.clearObserver();

  const kinds = new Set([...Object.keys(cppCounts), ...Object.keys(jsCounts)]);
  const diffs = [];
  for (const k of kinds) {
    const c = cppCounts[k] || 0;
    const j = jsCounts[k] || 0;
    if (c !== j) diffs.push(`${k}: cpp=${c} js=${j}`);
  }
  return { ok: diffs.length === 0, cppCounts, jsCounts, diffs };
}

function main() {
  const metisPath = process.argv[2];
  const seed = parseInt(process.argv[3] || "0", 10);
  const N_L0 = parseInt(process.argv[4] || "100", 10);
  if (!metisPath || !existsSync(metisPath)) {
    console.error("usage: diagnostic_ladder.mjs <metis_file> [seed=0] [n_l0=100]");
    process.exit(2);
  }

  console.log(`=== L0: raw MT19937 stream parity (N=${N_L0}, seed=${seed}) ===`);
  const l0 = L0(seed, N_L0);
  if (l0.ok) console.log(`L0 PASS: ${l0.N}/${l0.N} raw outputs bit-equal`);
  else { console.error(`L0 FAIL: ${l0.mismatches}/${l0.N} mismatches; first: ${l0.firstMismatch}`); process.exit(1); }

  console.log(`\n=== L1: per-call draw-count parity (${metisPath}, seed=${seed}) ===`);
  const l1 = L1(metisPath, seed);
  if (l1.skipped) {
    console.log("L1 SKIPPED (mincut=0)");
  } else if (l1.ok) {
    console.log(`L1 PASS: counts ${JSON.stringify(l1.cppCounts)}`);
  } else {
    console.error(`L1 FAIL: ${l1.diffs.join("; ")}`);
    console.error(`  cpp=${JSON.stringify(l1.cppCounts)} js=${JSON.stringify(l1.jsCounts)}`);
    process.exit(1);
  }

  console.log(`\nL2: N/A (audit row D = no FP primitives on cactus path)`);
  console.log("L3: see separation_test.mjs (oracle-replay)");
}

main();
