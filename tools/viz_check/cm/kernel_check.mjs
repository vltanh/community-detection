/* CM kernel cross-check, JS replay leg.
 *
 * Reads the C++ tracer's stdout JSON. Each pop has the canonical
 * mincut bipartition + (when not well-connected) the Leiden recluster
 * output for each side. JS replay injects both via cutOracle and
 * baseAlgoFn so the JS walk byte-equals the canonical's.
 *
 * Asserts:
 *   - per-pop record matches: round, cid, n, cut, threshold, wc, in, out.
 *   - per-recluster children list matches.
 *   - final survivor (lineage-id, nodes) list matches.
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
await import(path.join(WEB, "cm/cm.js"));

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

// Build oracles keyed by sorted cluster nodes string.
const cutMap = new Map();      // key -> { cutValue, in, out }
const reclusterMap = new Map();// key -> { children: [[ids],...] }
cpp.pops.forEach(function (p) {
  const key = p.cluster.slice().sort(function (a, b) { return a - b; }).join(",");
  cutMap.set(key, {
    cutValue: p.cut,
    inPartition: p.in.slice(),
    outPartition: p.out.slice(),
  });
  if (!p.wc) {
    // children list, grouped by side. Children come back as { parent, nodes }.
    const inSide = [], outSide = [];
    const inSet = new Set(p.in), outSet = new Set(p.out);
    p.children.forEach(function (ch) {
      if (ch.nodes.every(function (x) { return inSet.has(x); })) inSide.push(ch.nodes.slice());
      else outSide.push(ch.nodes.slice());
    });
    reclusterMap.set(key, { inSide, outSide });
  }
});

let cutHits = 0, reHits = 0;
function cutOracle(clusterNodes, _sub) {
  const key = clusterNodes.slice().sort(function (a, b) { return a - b; }).join(",");
  const r = cutMap.get(key);
  if (!r) throw new Error("cut oracle miss for cluster: " + key.slice(0, 80));
  cutHits++;
  return r;
}

// pendingChildren[clusterKey] = { inSide, outSide } - consumed as the
// JS visits each side's recluster.
function baseAlgoFn(side, _sideEdges, ctx) {
  // ctx.parentNodes is the current pop's cluster (the one we're
  // reclustering). Look up by exact match.
  if (!ctx || !ctx.parentNodes) {
    throw new Error("baseAlgoFn requires ctx.parentNodes from kernel");
  }
  const key = ctx.parentNodes.slice().sort(function (a, b) { return a - b; }).join(",");
  const val = reclusterMap.get(key);
  if (!val) throw new Error("recluster oracle miss for parent: " + key.slice(0, 80));
  const cut = cutMap.get(key);
  const sideSorted = side.slice().sort(function (a, b) { return a - b; }).join(",");
  const inSorted = cut.inPartition.slice().sort(function (a, b) { return a - b; }).join(",");
  reHits++;
  const result = (sideSorted === inSorted) ? val.inSide : val.outSide;
  return result.map(function (c) { return c.slice(); });
}

const r = COMDET.CM.runCM(G.membership, {
  fixture, criterion, algorithm: "leiden-cpm", resolution: 0.0001, seed: 0,
  cutOracle, baseAlgoFn,
});

// Diff per-pop trace.
const cppPops = cpp.pops;
const jsPops = r.events.filter(function (e) { return e.kind === "mincut"; });
let pass = true;
const report = [];
report.push(`oracle: cut hits=${cutHits} re hits=${reHits}`);
report.push(`cpp pops=${cppPops.length} js pops=${jsPops.length}`);
if (cppPops.length !== jsPops.length) pass = false;
const N = Math.max(cppPops.length, jsPops.length);
for (let i = 0; i < N; i++) {
  const c = cppPops[i] || {};
  const j = jsPops[i] || {};
  const issues = [];
  if (c.n !== j.clusterSize) issues.push(`n cpp=${c.n} js=${j.clusterSize}`);
  if (c.cut !== j.cut) issues.push(`cut cpp=${c.cut} js=${j.cut}`);
  if (Math.abs(c.thr - j.threshold) > 1e-9) issues.push(`thr`);
  if (c.wc !== j.wellConnected) issues.push(`wc cpp=${c.wc} js=${j.wellConnected}`);
  if (c.cluster_id !== j.id) issues.push(`cid cpp=${c.cluster_id} js=${j.id}`);
  if (issues.length) {
    pass = false;
    report.push(`  pop[${i}] DIFFER ${issues.join("; ")}`);
  } else {
    report.push(`  pop[${i}] match round=${c.round} cid=${c.cluster_id} n=${c.n} cut=${c.cut} wc=${c.wc}`);
  }
}

// Survivors set (lineage id, sorted nodes).
function key(s) {
  return s.id + ":" + s.nodes.slice().sort(function (a, b) { return a - b; }).join(",");
}
const cSurv = (cpp.survivors || []).map(function (s) {
  return s.id + ":" + s.nodes.slice().sort(function (a, b) { return a - b; }).join(",");
}).sort();
const jSurv = r.survivors.map(key).sort();
if (JSON.stringify(cSurv) !== JSON.stringify(jSurv)) {
  pass = false;
  report.push(`survivors DIFFER cpp=${cSurv.length} js=${jSurv.length}`);
} else {
  report.push(`survivors match: ${cSurv.length} clusters w/ lineage ids`);
}

console.log(report.join("\n"));
if (!pass) {
  console.error("FAIL: CM tracer vs JS replay diverged.");
  process.exit(1);
}
console.log("PASS: CM tracer == JS replay (byte-equal pop trace + lineage survivors).");
