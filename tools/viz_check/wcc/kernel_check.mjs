/* WCC kernel cross-check, JS replay leg.
 *
 * Reads the C++ tracer's stdout JSON. Each "pop" record contains the
 * canonical mincut bipartition (in[] + out[] in NEW node ids) and the
 * cut value. The JS replay loads comdet/js/wcc/wcc.js with a
 * cutOracle that, on each pop, looks up the matching bipartition by
 * cluster-nodes key and returns it. This makes the JS WCC walk
 * identical to the canonical's, byte-for-byte.
 *
 * Asserts:
 *   - per-pop record matches: same n, cut, threshold, wc, in, out, pushed.
 *   - final survivors list matches.
 *
 * Run:
 *   /tmp/wcc_kernel_check edge.csv com.csv js_out.csv "1log_10(n)" > cpp.json 2>cpp.trace
 *   node tools/viz_check/wcc/kernel_check.mjs cpp.json edge.csv com.csv "1log_10(n)"
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 3) {
  console.error("usage: kernel_check.mjs <cpp_trace.json> <edge.csv> <com.csv> [criterion]");
  process.exit(2);
}
const [cppPath, edgePath, comPath, criterion = "1log_10(n)"] = args;

const cpp = JSON.parse(fs.readFileSync(cppPath, "utf8"));

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = path.join(__dirname, "../../../../../web/vltanh.github.io/comdet/js");
await import(path.join(WEB, "mincut.js"));
await import(path.join(WEB, "louvain/louvain.js"));
await import(path.join(WEB, "leiden/leiden.js"));
await import(path.join(WEB, "comdet/page_helpers.js"));
await import(path.join(WEB, "wcc/wcc.js"));

function loadGraph(edgePath, comPath) {
  const edgeText = fs.readFileSync(edgePath, "utf8").trim().split(/\r?\n/);
  const com = fs.readFileSync(comPath, "utf8").trim().split(/\r?\n/);
  const orig_to_new = new Map();
  const edges = [];
  for (let i = 1; i < edgeText.length; i++) {
    if (!edgeText[i]) continue;
    const cols = edgeText[i].split(/[,\t ]/);
    const s = cols[0], t = cols[1];
    if (!orig_to_new.has(s)) orig_to_new.set(s, orig_to_new.size);
    if (!orig_to_new.has(t)) orig_to_new.set(t, orig_to_new.size);
    edges.push([orig_to_new.get(s), orig_to_new.get(t)]);
  }
  const partition = new Int32Array(orig_to_new.size);
  for (let i = 0; i < partition.length; i++) partition[i] = -2147483648;
  for (let i = 1; i < com.length; i++) {
    if (!com[i]) continue;
    const cols = com[i].split(/[,\t ]/);
    const newId = orig_to_new.get(cols[0]);
    if (newId == null) continue;
    partition[newId] = parseInt(cols[1], 10);
  }
  return {
    nodes: Array.from({ length: orig_to_new.size }, (_, i) => i),
    edges,
    membership: partition,
  };
}

const G = loadGraph(edgePath, comPath);
const fixture = { nodes: G.nodes, edges: G.edges, gt: Array.from(G.membership) };

// Build oracle keyed by sorted-cluster-nodes string -> {cutValue, in, out}.
const oracle = new Map();
cpp.pops.forEach(function (p) {
  const key = p.cluster.slice().sort(function (a, b) { return a - b; }).join(",");
  oracle.set(key, { cutValue: p.cut, inPartition: p.in.slice(), outPartition: p.out.slice() });
});

let oracleHits = 0, oracleMiss = 0;
function cutOracle(clusterNodes, _sub) {
  const key = clusterNodes.slice().sort(function (a, b) { return a - b; }).join(",");
  const r = oracle.get(key);
  if (!r) {
    oracleMiss++;
    throw new Error("oracle miss for cluster: " + key.slice(0, 80));
  }
  oracleHits++;
  return r;
}

const r = COMDET.WCC.runWCC(G.membership, {
  fixture, criterion,
  cutOracle,
  trace: false,
});

// Compare every pop record to cpp.pops in order.
const cppPops = cpp.pops;
const jsPops = r.carve.events;
let pass = true;
const report = [];
report.push(`oracle hits=${oracleHits} miss=${oracleMiss}`);
report.push(`cpp pops=${cppPops.length} js pops=${jsPops.length}`);
if (cppPops.length !== jsPops.length) pass = false;
const N = Math.max(cppPops.length, jsPops.length);
for (let i = 0; i < N; i++) {
  const c = cppPops[i] || {};
  const j = jsPops[i] || {};
  const issues = [];
  if (c.n !== j.clusterSize) issues.push(`n cpp=${c.n} js=${j.clusterSize}`);
  if (c.cut !== j.cut) issues.push(`cut cpp=${c.cut} js=${j.cut}`);
  if (Math.abs(c.thr - j.threshold) > 1e-9) issues.push(`thr cpp=${c.thr} js=${j.threshold}`);
  if (c.wc !== j.wellConnected) issues.push(`wc cpp=${c.wc} js=${j.wellConnected}`);
  const cIn = (c.in || []).slice().sort(function (a, b) { return a - b; });
  const jIn = (j.inPartition || []).slice().sort(function (a, b) { return a - b; });
  if (JSON.stringify(cIn) !== JSON.stringify(jIn)) issues.push(`in_set differs`);
  const cOut = (c.out || []).slice().sort(function (a, b) { return a - b; });
  const jOut = (j.outPartition || []).slice().sort(function (a, b) { return a - b; });
  if (JSON.stringify(cOut) !== JSON.stringify(jOut)) issues.push(`out_set differs`);
  if (issues.length) {
    pass = false;
    report.push(`  pop[${i}] DIFFER ${issues.join("; ")}`);
  } else {
    report.push(`  pop[${i}] match n=${c.n} cut=${c.cut} wc=${c.wc}`);
  }
}
// Final survivors.
const cSurv = (cpp.survivors || []).map(function (s) {
  return s.slice().sort(function (a, b) { return a - b; }).join(",");
}).sort();
const jSurv = r.survivors.map(function (s) {
  return s.slice().sort(function (a, b) { return a - b; }).join(",");
}).sort();
if (JSON.stringify(cSurv) !== JSON.stringify(jSurv)) {
  pass = false;
  report.push(`survivors set DIFFER: cpp=${cSurv.length} js=${jSurv.length}`);
} else {
  report.push(`survivors match: ${cSurv.length} clusters`);
}

console.log(report.join("\n"));
if (!pass) {
  console.error("FAIL: WCC tracer vs JS replay diverged.");
  process.exit(1);
}
console.log("PASS: WCC tracer == JS replay (byte-equal pop trace + survivors).");
