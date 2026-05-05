/* Infomap JS-vs-canonical structural cross-check.
 *
 * NOT 3-leg byte-equal. JS Infomap (comdet/js/infomap/infomap.js) is a
 * faithful undirected-unweighted port of Rosvall + Bergstrom 2008 with
 * GREEDY pair-joining + GREEDY single-node tuning (paper SI suggests
 * heat-bath simulated-annealing; the JS kernel comments flag the
 * divergence). Canonical Infomap (pypi C++) uses different optimisation
 * heuristics + supports many more features. The two are different
 * algorithmic instantiations of the same paper, so byte-equal partition
 * is impossible by design.
 *
 * What this verifier does instead:
 *   - Runs JS runInfomap with --seed (deterministic per the kernel's
 *     own MT19937 path; greedy pipeline is seed-free but renumber +
 *     sublevel are deterministic).
 *   - Computes JS map-equation L on the JS partition.
 *   - Reports L_canon (from canonical com.csv leg) vs L_js (computed by
 *     JS's own mapEquation on JS's own partition).
 *   - Reports L_js_on_canon: JS's map-equation evaluated on the
 *     canonical partition. Validates JS map-equation formula matches
 *     canonical's (both should land near canonical-Infomap's reported
 *     codelength on the canonical partition).
 *   - Reports ARI between JS and canonical partitions.
 *
 * Acceptance: |L_js_on_canon - L_canon| < 1e-3 (validates JS map-eq
 * formula vs canonical's L). Partition similarity is reported but not
 * gated.
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 4) {
  console.error("usage: kernel_check.mjs <canonical.csv> <canonical_stats.json> <edge.csv> <seed>");
  process.exit(2);
}
const [canonCsv, canonStatsPath, edgePath, seedStr] = args;
const seed = parseInt(seedStr, 10);

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = path.join(__dirname, "../../../vltanh.github.io/comdet/js");
await import(path.join(WEB, "louvain/louvain.js"));
await import(path.join(WEB, "infomap/infomap.js"));

function loadEdges(edgePath) {
  const lines = fs.readFileSync(edgePath, "utf8").trim().split(/\r?\n/);
  const edges = [];
  const seen = new Set();
  const nodes = [];
  for (let i = 1; i < lines.length; i++) {
    if (!lines[i]) continue;
    const cols = lines[i].split(/[,\t ]/);
    const u = parseInt(cols[0], 10);
    const v = parseInt(cols[1], 10);
    if (u === v) continue;
    if (!seen.has(u)) { seen.add(u); nodes.push(u); }
    if (!seen.has(v)) { seen.add(v); nodes.push(v); }
    edges.push([u, v]);
  }
  return { nodes, edges };
}

function loadCanonCsv(p) {
  const lines = fs.readFileSync(p, "utf8").trim().split(/\r?\n/);
  const m = new Map();
  for (let i = 1; i < lines.length; i++) {
    const cols = lines[i].split(",");
    m.set(parseInt(cols[0], 10), parseInt(cols[1], 10));
  }
  return m;
}

function ari(memA, memB, nodes) {
  // Adjusted Rand Index. Treat unmapped nodes (singletons) as own cluster.
  const labA = new Map();
  const labB = new Map();
  let aNext = 0, bNext = 0;
  function getOrCreate(map, k, counter) {
    if (!map.has(k)) {
      map.set(k, counter.v);
      counter.v += 1;
    }
    return map.get(k);
  }
  const cA = { v: 0 };
  const cB = { v: 0 };
  const arrA = [];
  const arrB = [];
  nodes.forEach(n => {
    const a = memA.has(n) ? "c" + memA.get(n) : "s_a_" + n;
    const b = memB.has(n) ? "c" + memB.get(n) : "s_b_" + n;
    arrA.push(getOrCreate(labA, a, cA));
    arrB.push(getOrCreate(labB, b, cB));
  });
  const nA = cA.v, nB = cB.v;
  const tab = new Array(nA);
  for (let i = 0; i < nA; i++) tab[i] = new Array(nB).fill(0);
  for (let i = 0; i < arrA.length; i++) tab[arrA[i]][arrB[i]] += 1;
  const sumA = new Array(nA).fill(0);
  const sumB = new Array(nB).fill(0);
  for (let i = 0; i < nA; i++) for (let j = 0; j < nB; j++) {
    sumA[i] += tab[i][j];
    sumB[j] += tab[i][j];
  }
  function comb2(x) { return x < 2 ? 0 : x * (x - 1) / 2; }
  let sumNijC2 = 0;
  for (let i = 0; i < nA; i++) for (let j = 0; j < nB; j++) sumNijC2 += comb2(tab[i][j]);
  let sumAiC2 = 0;
  sumA.forEach(x => sumAiC2 += comb2(x));
  let sumBjC2 = 0;
  sumB.forEach(x => sumBjC2 += comb2(x));
  const N = arrA.length;
  const Nc2 = comb2(N);
  const expected = (sumAiC2 * sumBjC2) / Nc2;
  const max = (sumAiC2 + sumBjC2) / 2;
  if (max - expected === 0) return 1;
  return (sumNijC2 - expected) / (max - expected);
}

const { nodes, edges } = loadEdges(edgePath);
const canonStats = JSON.parse(fs.readFileSync(canonStatsPath, "utf8"));
const canonMem = loadCanonCsv(canonCsv);

// JS Infomap.
const res = COMDET.INFOMAP.runInfomap(nodes, edges, {
  seed: seed,
  recurse: false,
  recordTrace: false,
});
const L_js = res.finalL;
const memJs = res.membership;

// JS map-equation evaluated on canonical partition. Build a partition
// array indexed by JS internal compact node id. JS runInfomap uses
// nodeIds order for compaction.
const idxOf = new Map();
nodes.forEach((id, i) => idxOf.set(id, i));
const canonPartArr = new Array(nodes.length);
let nextSing = 0;
const canonRemap = new Map();
nodes.forEach((id, i) => {
  if (canonMem.has(id)) {
    const c = canonMem.get(id);
    if (!canonRemap.has(c)) canonRemap.set(c, canonRemap.size);
    canonPartArr[i] = canonRemap.get(c);
  } else {
    // Singleton — make a unique cluster id.
    canonPartArr[i] = canonRemap.size + (nextSing++);
  }
});
const g = COMDET.INFOMAP.buildGraph(nodes, edges);
const p = COMDET.INFOMAP.stationary(g);
const L_js_on_canon = COMDET.INFOMAP.mapEquation(g, p, canonPartArr).L;

const aR = ari(memJs, canonMem, nodes);

const out = {
  L_canon: canonStats.codelength,
  L_js: L_js,
  L_js_on_canon: L_js_on_canon,
  ari: aR,
  n_nodes: nodes.length,
  n_edges: edges.length,
  canon_clusters: canonStats.n_kept_clusters,
};

console.log("=== Infomap structural cross-check ===");
console.log(`L_canon       (canonical pypi C++ on canonical partition) = ${canonStats.codelength.toFixed(6)}`);
console.log(`L_js          (JS algo + JS map-eq on JS partition)       = ${L_js.toFixed(6)}`);
console.log(`L_js_on_canon (JS map-eq on canonical partition)          = ${L_js_on_canon.toFixed(6)}`);
console.log(`ARI(canonical, JS) = ${aR.toFixed(4)}`);
console.log(`canonical kept clusters: ${canonStats.n_kept_clusters}`);

const dL = Math.abs(L_js_on_canon - canonStats.codelength);
if (dL > 1e-3) {
  console.log(`FAIL: |L_js_on_canon - L_canon| = ${dL.toExponential(3)} > 1e-3.`);
  console.log("This means JS map-equation formula does NOT match canonical's.");
  process.exit(1);
}
console.log(`PASS: |L_js_on_canon - L_canon| = ${dL.toExponential(3)} <= 1e-3 (JS map-eq formula matches canonical).`);
process.exit(0);
