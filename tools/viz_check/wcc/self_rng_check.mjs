/* WCC self-RNG L4 check.
 *
 * No oracles. JS production walker runs own VieCut backend (own MT19937)
 * + own Math.log; compares per-pop record bit-equal vs canonical-tracer
 * (swapped build) trace. Threshold doubles compared via uint64 reinterpret.
 *
 * Run:
 *   /tmp/wcc_kernel_check_swapped <edge.csv> <com.csv> /tmp/_.csv "1log_10(n)" cactus <seed>
 *      > /tmp/wcc_swap.json 2>/tmp/wcc_swap.err
 *   node tools/viz_check/wcc/self_rng_check.mjs /tmp/wcc_swap.json <edge.csv> <com.csv> "1log_10(n)" <seed>
 *
 * Exit: 0 PASS, 1 FAIL.
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 3) {
  console.error("usage: self_rng_check.mjs <cpp_swap.json> <edge.csv> <com.csv> [criterion] [seed]");
  process.exit(2);
}
const [cppPath, edgePath, comPath, criterion = "1log_10(n)", seedStr = "0"] = args;
const seed = parseInt(seedStr, 10);

function bits(x) {
  const b = new Float64Array(1); b[0] = x;
  return new BigUint64Array(b.buffer)[0];
}

const cpp = JSON.parse(fs.readFileSync(cppPath, "utf8"));

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = path.join(__dirname, "../../../../../web/vltanh.github.io/comdet/js");
await import(path.join(WEB, "mincut.js"));
await import(path.join(WEB, "viecut/random.js"));
await import(path.join(WEB, "viecut/union_find.js"));
await import(path.join(WEB, "viecut/node_bucket_pq.js"));
await import(path.join(WEB, "viecut/fifo_node_bucket_pq.js"));
await import(path.join(WEB, "viecut/mutable_graph.js"));
await import(path.join(WEB, "viecut/cactus_graph.js"));
await import(path.join(WEB, "viecut/scc.js"));
await import(path.join(WEB, "viecut/balanced_cut_dfs.js"));
await import(path.join(WEB, "viecut/most_balanced.js"));
await import(path.join(WEB, "viecut/minimum_cut_helpers.js"));
await import(path.join(WEB, "viecut/contract_graph.js"));
await import(path.join(WEB, "viecut/contraction_tests.js"));
await import(path.join(WEB, "viecut/noi_minimum_cut.js"));
await import(path.join(WEB, "viecut/viecut_heuristic.js"));
await import(path.join(WEB, "viecut/push_relabel.js"));
await import(path.join(WEB, "viecut/heavy_edges.js"));
await import(path.join(WEB, "viecut/graph_modification.js"));
await import(path.join(WEB, "viecut/recursive_cactus.js"));
await import(path.join(WEB, "viecut/cactus_mincut.js"));
await import(path.join(WEB, "viecut/mincut_adapter.js"));
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

// Seed JS-side VieCut RNG. mincut_adapter.js exposes `setSeed`.
if (COMDET.MINCUT && COMDET.MINCUT.viecut && COMDET.MINCUT.viecut.setSeed) {
  COMDET.MINCUT.viecut.setSeed(seed);
}

const r = COMDET.WCC.runWCC(G.membership, {
  fixture, criterion,
  trace: false,
  // No cutOracle; JS runs own VieCut.
});

const cppPops = cpp.pops;
const jsPops = r.carve.events;

let pass = true;
const report = [];
report.push(`canonical: pops=${cppPops.length} survivors=${cpp.survivors.length}`);
report.push(`js:        pops=${jsPops.length} survivors=${r.survivors.length}`);

const N = Math.min(cppPops.length, jsPops.length);
let nThrBitMM = 0, nClusterMM = 0, nCutMM = 0, nWCMM = 0, nInMM = 0, nOutMM = 0;
let nFirstMM = -1;
for (let i = 0; i < N; i++) {
  const c = cppPops[i], j = jsPops[i];
  let cellOK = true;
  // n / cluster size.
  if (c.n !== j.clusterSize) { nClusterMM++; cellOK = false; }
  // cut value.
  if (c.cut !== j.cut) { nCutMM++; cellOK = false; }
  // wc verdict.
  if (c.wc !== j.wellConnected) { nWCMM++; cellOK = false; }
  // threshold bit-equal via uint64 reinterpret.
  const cBits = BigInt(c.thr_bits);
  const jBits = bits(j.threshold);
  if (cBits !== jBits) { nThrBitMM++; cellOK = false; }
  // in/out partitions: order-sensitive (canonical emits in node-id order
  // via VieCut's getNodeInCut iteration; JS port mirrors).
  const cIn = c.in.slice(), jIn = j.inPartition.slice();
  if (JSON.stringify(cIn) !== JSON.stringify(jIn)) { nInMM++; cellOK = false; }
  const cOut = c.out.slice(), jOut = j.outPartition.slice();
  if (JSON.stringify(cOut) !== JSON.stringify(jOut)) { nOutMM++; cellOK = false; }
  if (!cellOK && nFirstMM < 0) nFirstMM = i;
}

report.push(`per-pop:  thr_bit_mm=${nThrBitMM}/${N} cluster_mm=${nClusterMM} cut_mm=${nCutMM} wc_mm=${nWCMM} in_mm=${nInMM} out_mm=${nOutMM}`);
if (nFirstMM >= 0) {
  const c = cppPops[nFirstMM], j = jsPops[nFirstMM];
  report.push(`first divergent pop[${nFirstMM}]: cpp(n=${c.n} cut=${c.cut} thr_bits=${c.thr_bits} wc=${c.wc}) js(n=${j.clusterSize} cut=${j.cut} thr_bits=0x${bits(j.threshold).toString(16).padStart(16,'0')} wc=${j.wellConnected})`);
  pass = false;
}
if (cppPops.length !== jsPops.length) {
  report.push(`pop_count_mm: cpp=${cppPops.length} js=${jsPops.length}`);
  pass = false;
}

// Survivors set comparison (sorted node-id within cluster, sorted clusters).
const cSurv = (cpp.survivors || []).map(s => s.slice().sort((a,b)=>a-b).join(",")).sort();
const jSurv = r.survivors.map(s => s.slice().sort((a,b)=>a-b).join(",")).sort();
if (JSON.stringify(cSurv) !== JSON.stringify(jSurv)) {
  report.push(`survivors_set_mm: cpp=${cSurv.length} js=${jSurv.length}`);
  pass = false;
} else {
  report.push(`survivors_set: PASS (${cSurv.length} clusters)`);
}

console.log(report.join("\n"));
if (pass) {
  console.log(`PASS: WCC self-RNG L4 byte-equal, ${N}/${N} pops bit-identical`);
  process.exit(0);
} else {
  console.error(`FAIL: WCC self-RNG L4`);
  process.exit(1);
}
