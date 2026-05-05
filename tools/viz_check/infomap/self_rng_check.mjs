/* L4 self-RNG end-to-end: drive COMDET.INFOMAP_CANON.runInfomapCanonical with
 * seed N (no oracle), post-process the partition to match canonical_run.py's
 * shape (drop singletons + renumber 0..K-1 + sort-by-node_id), diff against
 * the canonical com.csv.
 *
 * Usage:
 *   node self_rng_check.mjs <edge.csv> <seed> <canonical_com.csv> [<canonical_stats.json>]
 *
 * Exits 0 with PASS if JS partition byte-equals canonical (and L within 1e-9
 * if stats provided), 1 otherwise.
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 3) {
  console.error("usage: self_rng_check.mjs <edge.csv> <seed> <canonical_com.csv> [<canonical_stats.json>]");
  process.exit(2);
}
const [edgeCsv, seedArg, canonCsv, statsJson] = args;
const seed = parseInt(seedArg, 10) >>> 0;

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = path.join(__dirname, "../../../vltanh.github.io/comdet/js");
await import(path.join(WEB, "louvain/louvain.js"));
await import(path.join(WEB, "infomap/infomap_canon.js"));

// Parse edge.csv exactly as canonical_run.py does:
//   pd.unique(df[[src,tgt]].values.ravel("K")) — column-major flatten,
//   first-encounter order. node_map = {n: i for i, n in enumerate(nodes)}.
const lines = fs.readFileSync(edgeCsv, "utf-8").trim().split(/\r?\n/);
// Skip header
const dataLines = lines.slice(1);
// Parse edges as strings first to preserve raveled column-major order
const srcs = [], tgts = [];
for (const ln of dataLines) {
  const [s, t] = ln.split(",");
  srcs.push(s); tgts.push(t);
}
// pandas ravel("K") = column-major: walk down column 0, then column 1.
const seen = new Map();
const nodes = [];
function addNode(s) {
  if (!seen.has(s)) { seen.set(s, nodes.length); nodes.push(s); }
}
for (const s of srcs) addNode(s);
for (const t of tgts) addNode(t);

const nodeIdToIdx = seen;
const edges = [];
for (let i = 0; i < srcs.length; i++) {
  edges.push([nodeIdToIdx.get(srcs[i]), nodeIdToIdx.get(tgts[i])]);
}
// runInfomapCanonical takes node ids (we use compact 0..n-1) + edges as
// pairs of those.
const compactIds = nodes.map((_, i) => i);

const t0 = Date.now();
const res = COMDET.INFOMAP_CANON.runInfomapFaithful(compactIds, edges, {
  seed: seed,
  aggregationLimit: 30,
});
const elapsed = (Date.now() - t0) / 1000;

// Build (orig_node_id, cluster) rows.
const rows = [];
for (let i = 0; i < nodes.length; i++) {
  const origNumeric = parseInt(nodes[i], 10);
  rows.push([origNumeric, res.finalPartition[i]]);
}

// Drop singletons + renumber 0..K-1 by first-encounter-in-nodeid-order
// (label-invariant: any two equivalent partitions canonicalize to the same
// CSV regardless of the kernel's raw cluster ID convention).
function canonicalize(rows) {
  const cnt = new Map();
  for (const [, c] of rows) cnt.set(c, (cnt.get(c) || 0) + 1);
  const filtered = rows.filter(([, c]) => cnt.get(c) > 1)
    .sort((a, b) => a[0] - b[0]);
  const remap = new Map();
  for (const [, c] of filtered) {
    if (!remap.has(c)) remap.set(c, remap.size);
  }
  let csv = "node_id,cluster_id\n";
  for (const [n, c] of filtered) csv += `${n},${remap.get(c)}\n`;
  return csv;
}
const jsCsv = canonicalize(rows);

// Re-canonicalize the canonical CSV the same way (its raw labels follow a
// different convention; both must be normalized to first-encounter to
// allow byte-equal comparison).
const canonRaw = fs.readFileSync(canonCsv, "utf-8").trim().split("\n").slice(1);
const canonRows = canonRaw.map(ln => {
  const [n, c] = ln.split(",");
  return [parseInt(n, 10), parseInt(c, 10)];
});
const canonText = canonicalize(canonRows);
const partitionMatch = (jsCsv === canonText);

// L diff (if stats provided)
let lOk = true, dL = null, lCanon = null;
if (statsJson && fs.existsSync(statsJson)) {
  const stats = JSON.parse(fs.readFileSync(statsJson, "utf-8"));
  lCanon = stats.codelength;
  dL = Math.abs(res.finalL - lCanon);
  lOk = dL <= 1e-9;
}

console.log(`  seed=${seed}  n=${nodes.length}  m=${edges.length}  elapsed=${elapsed.toFixed(2)}s`);
console.log(`  JS L=${res.finalL.toFixed(15)}` + (lCanon !== null ? `  L_canon=${lCanon.toFixed(15)}  |ΔL|=${dL.toExponential(3)}` : ""));
const jsRowCount = jsCsv.trim().split("\n").length - 1;
const canonRowCount = canonText.trim().split("\n").length - 1;
console.log(`  partition match: ${partitionMatch ? "PASS" : "FAIL"} (js=${jsRowCount} kept rows; canon=${canonRowCount})`);
if (!partitionMatch) {
  // Show first divergent rows.
  const jsLines = jsCsv.trim().split("\n");
  const cnLines = canonText.trim().split("\n");
  let diffs = 0;
  for (let i = 0; i < Math.max(jsLines.length, cnLines.length) && diffs < 10; i++) {
    if (jsLines[i] !== cnLines[i]) {
      console.log(`    line ${i}: js=${jsLines[i]}  canon=${cnLines[i]}`);
      diffs++;
    }
  }
}

if (partitionMatch && lOk) {
  console.log("  OVERALL: PASS (self-RNG partition + L byte-equal canonical)");
  process.exit(0);
}
console.log("  OVERALL: FAIL");
process.exit(1);
