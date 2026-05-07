/* CM L4 self-RNG end-to-end diff harness.
 *
 * Verification target (per byte-equal-tracer skill, three-claims #1):
 *   JS visualizer == canonical-tracer (TRACER_MODE swapped build) bit-for-bit
 *   under matching seed.
 *
 * Drops cutOracle + baseAlgoFn from JS replay -- the JS CM runs its own
 * VieCut + own Leiden chain. Compares per-pop record bit-equal vs the
 * canonical-tracer's emitted trace using uint64 reinterpret on every double.
 *
 * Run:
 *   /tmp/cm_kernel_check_swapped <edge.csv> <com.csv> <out.csv> "1log_10(n)" 0.0001 <seed>
 *     > cpp_swapped.json 2>cpp_swapped.err
 *   node tools/viz_check/cm/self_rng_check.mjs cpp_swapped.json <edge.csv> <com.csv>
 *
 * Exits 0 on byte-equal; 1 on any mismatch (first divergent record reported).
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 3) {
  console.error("usage: self_rng_check.mjs <cpp_trace.json> <edge.csv> <com.csv> "
              + "[criterion] [resolution] [seed]");
  process.exit(2);
}
const [cppPath, edgePath, comPath,
       criterion = "1log_10(n)",
       resolution = 0.0001,
       seed = 0] = args;

const cpp = JSON.parse(fs.readFileSync(cppPath, "utf8"));
if (cpp.build !== "TRACER_MODE") {
  console.error(`WARN: cpp trace has build="${cpp.build}" (expected TRACER_MODE for L4 self-RNG check)`);
}

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = path.join(__dirname, "../../../../../web/vltanh.github.io/comdet/js");
await import(path.join(WEB, "mincut.js"));
await import(path.join(WEB, "louvain/louvain.js"));
await import(path.join(WEB, "leiden/leiden.js"));
await import(path.join(WEB, "comdet/page_helpers.js"));
await import(path.join(WEB, "wcc/wcc.js"));
// Load VieCut JS port if available so cm.js's mincutFn defaults to viecut.
// VieCut module loader pattern lives at viz_check/viecut/_loader.mjs.
try {
  const vcLoader = path.join(__dirname, "../viecut/_loader.mjs");
  if (fs.existsSync(vcLoader)) await import(vcLoader);
} catch (e) { /* fallback to stoer-wagner */ }
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

// uint64 reinterpret per skill playbook §3.
function bits(x) {
  const b = new Float64Array(1); b[0] = x;
  return new BigUint64Array(b.buffer)[0];
}

const G = loadGraph(edgePath, comPath);
const fixture = { nodes: G.nodes, edges: G.edges, gt: Array.from(G.membership) };

// L4: NO cutOracle, NO baseAlgoFn. JS runs own chain.
const r = COMDET.CM.runCM(G.membership, {
  fixture,
  criterion,
  algorithm: "leiden-cpm",
  resolution: parseFloat(resolution),
  seed: parseInt(seed, 10) >>> 0,
});

// Compare per-pop trace.
const cppPops = cpp.pops;
const jsPops = r.events.filter(function (e) { return e.kind === "mincut"; });
let pass = true;
const report = [];
report.push(`=== L4 self-RNG check (no oracles) ===`);
report.push(`cpp build=${cpp.build} cpp pops=${cppPops.length} js pops=${jsPops.length}`);

// Stop at first divergent record.
const N = Math.min(cppPops.length, jsPops.length);
let firstDiv = -1;
let divDetails = "";
for (let i = 0; i < N; i++) {
  const c = cppPops[i], j = jsPops[i];
  // Field-by-field bit-compare.
  if (c.round !== j.round) { firstDiv = i; divDetails = `round cpp=${c.round} js=${j.round}`; break; }
  if (c.cluster_id !== j.id) { firstDiv = i; divDetails = `cid cpp=${c.cluster_id} js=${j.id}`; break; }
  if (c.n !== j.clusterSize) { firstDiv = i; divDetails = `n cpp=${c.n} js=${j.clusterSize}`; break; }
  if (c.cut !== j.cut) { firstDiv = i; divDetails = `cut cpp=${c.cut} js=${j.cut}`; break; }
  if (bits(c.thr) !== bits(j.threshold)) {
    firstDiv = i;
    divDetails = `thr_bits cpp=0x${bits(c.thr).toString(16)} js=0x${bits(j.threshold).toString(16)}`;
    break;
  }
  if (c.wc !== j.wellConnected) { firstDiv = i; divDetails = `wc cpp=${c.wc} js=${j.wellConnected}`; break; }
  // Bipartition compare (id-list bit-equal under sorted-asc semantics).
  const cIn = c.in.slice().sort((a,b)=>a-b);
  const jIn = j.inPartition.slice().sort((a,b)=>a-b);
  if (cIn.length !== jIn.length || !cIn.every((x,k)=>x===jIn[k])) {
    firstDiv = i;
    divDetails = `in_partition cpp=[${cIn.slice(0,5).join(",")}...] (n=${cIn.length}) js=[${jIn.slice(0,5).join(",")}...] (n=${jIn.length})`;
    break;
  }
}

if (firstDiv >= 0) {
  pass = false;
  const c = cppPops[firstDiv], j = jsPops[firstDiv];
  report.push(`FIRST DIVERGENCE at pop[${firstDiv}]: ${divDetails}`);
  report.push(`  cpp: round=${c.round} cid=${c.cluster_id} n=${c.n} cut=${c.cut} thr=${c.thr} wc=${c.wc}`);
  report.push(`  js : round=${j.round} cid=${j.id}    n=${j.clusterSize} cut=${j.cut} thr=${j.threshold} wc=${j.wellConnected}`);
} else if (cppPops.length !== jsPops.length) {
  pass = false;
  report.push(`pop count differs: cpp=${cppPops.length} js=${jsPops.length}`);
} else {
  report.push(`per-pop trace bit-equal across ${N} records.`);
}

// Survivors compare.
const cSurv = (cpp.survivors || []).map(function (s) {
  return s.id + ":" + s.nodes.slice().sort((a,b)=>a-b).join(",");
}).sort();
const jSurv = r.survivors.map(function (s) {
  return s.id + ":" + s.nodes.slice().sort((a,b)=>a-b).join(",");
}).sort();
if (JSON.stringify(cSurv) !== JSON.stringify(jSurv)) {
  pass = false;
  report.push(`survivors DIFFER cpp=${cSurv.length} js=${jSurv.length}`);
} else {
  report.push(`survivors match: ${cSurv.length} clusters w/ lineage ids`);
}

console.log(report.join("\n"));
if (!pass) {
  console.error("FAIL: CM self-RNG diverged.");
  process.exit(1);
}
console.log("PASS: JS visualizer == canonical-tracer (TRACER_MODE) bit-for-bit.");
