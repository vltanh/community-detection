/* Per-sweep post-sweep moduleOf diff: cpp tracer vs JS faithful kernel.
 *
 * cpp emits `partition_end[i] = m_activeNetwork[i]->index` at end of every
 * tryMoveEachNodeIntoBestModule call (= one sweep). JS emits the same via
 * __INFOMAP_PARTITION_DUMP at end of every tryMoveEach call inside
 * optimizeActiveNetwork. Walks both lockstep and prints the FIRST sweep
 * where moduleOf diverges between sides.
 *
 * Useful when visit_decision_diff.mjs reports all per-visit decisions
 * byte-equal but the final partition still diverges -- catches ID-relabel
 * or drag-target drift that visit-level probing misses.
 *
 * Usage:
 *   node partition_diff_per_call.mjs <cpp_tracer.json> <edge.csv> <seed>
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 3) {
  console.error("usage: partition_diff_per_call.mjs <cpp_tracer.json> <edge.csv> <seed>");
  process.exit(2);
}
const [tracerPath, edgeCsv, seedArg] = args;
const seed = parseInt(seedArg, 10) >>> 0;

let raw = fs.readFileSync(tracerPath, "utf-8");
if (raw[0] !== "{") raw = raw.slice(raw.indexOf("\n") + 1);
raw = raw.slice(0, raw.lastIndexOf("}") + 1);
const cpp = JSON.parse(raw);

const cppCalls = cpp.calls || [];
console.log(`cpp calls captured: ${cppCalls.length}`);

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = path.join(__dirname, "../../../vltanh.github.io/comdet/js");
await import(path.join(WEB, "louvain/louvain.js"));
await import(path.join(WEB, "infomap/infomap_canon.js"));

const jsDumps = [];
globalThis.__INFOMAP_PARTITION_DUMP = function(rec) {
  jsDumps.push({
    n: rec.n,
    nMoved: rec.nMoved,
    L: rec.L,
    moduleOf: rec.moduleOf.slice(),
  });
};

// Read edges + run.
const lines = fs.readFileSync(edgeCsv, "utf-8").trim().split(/\r?\n/);
const data = lines.slice(1);
const srcs = [], tgts = [];
for (const ln of data) {
  const [s, t] = ln.split(",");
  srcs.push(s); tgts.push(t);
}
const seen = new Map();
const nodes = [];
function addNode(s) { if (!seen.has(s)) { seen.set(s, nodes.length); nodes.push(s); } }
for (const s of srcs) addNode(s);
for (const t of tgts) addNode(t);
const edges = [];
for (let i = 0; i < srcs.length; i++) edges.push([seen.get(srcs[i]), seen.get(tgts[i])]);
const compactIds = nodes.map((_, i) => i);

const t0 = Date.now();
const res = COMDET.INFOMAP_CANON.runInfomapFaithful(compactIds, edges, {
  seed: seed,
  aggregationLimit: 30,
});
const elapsed = (Date.now() - t0) / 1000;
console.log(`js  dumps captured: ${jsDumps.length} (in ${elapsed.toFixed(2)}s)`);
console.log(`js  finalL=${res.finalL.toFixed(15)}  cpp finalL=${cpp.L_canon.toFixed(15)}`);

const N = Math.min(cppCalls.length, jsDumps.length);
console.log(`comparing ${N} calls (cpp=${cppCalls.length} js=${jsDumps.length})`);

let firstCall = -1, firstField = null, firstIdx = -1;
let cppV = 0, jsV = 0;
for (let k = 0; k < N; k++) {
  const c = cppCalls[k];
  const j = jsDumps[k];
  if (!c.partition_end || c.partition_end.length === 0) continue;
  // Compare nMoved + size first.
  if ((c.nMoved | 0) !== (j.nMoved | 0)) {
    console.log(`call ${k}: nMoved cpp=${c.nMoved} js=${j.nMoved} (lvl=${c.l} fl=${c.fl})`);
    if (firstCall < 0) { firstCall = k; firstField = "nMoved"; firstIdx = -1; cppV = c.nMoved; jsV = j.nMoved; }
  }
  if (c.partition_end.length !== j.moduleOf.length) {
    console.log(`call ${k}: partition_end size cpp=${c.partition_end.length} js=${j.moduleOf.length}`);
    continue;
  }
  let mismatchedIdx = -1;
  for (let i = 0; i < c.partition_end.length; i++) {
    if (c.partition_end[i] !== j.moduleOf[i]) {
      mismatchedIdx = i;
      break;
    }
  }
  if (mismatchedIdx >= 0 && firstCall < 0) {
    firstCall = k;
    firstField = "moduleOf";
    firstIdx = mismatchedIdx;
    cppV = c.partition_end[mismatchedIdx];
    jsV = j.moduleOf[mismatchedIdx];
  }
}

if (firstCall < 0) {
  console.log(`ALL ${N} call partition_ends byte-equal`);
  process.exit(0);
}

const c = cppCalls[firstCall];
const j = jsDumps[firstCall];
console.log(`\nFIRST DIVERGENCE at call=${firstCall} field=${firstField} idx=${firstIdx}`);
console.log(`  cpp: lvl=${c.l} fl=${c.fl} nMoved=${c.nMoved} L_post=${c.L_post}`);
console.log(`  js : nMoved=${j.nMoved} L=${j.L}`);
if (firstField === "moduleOf") {
  console.log(`  cpp partition_end[${firstIdx}] = ${cppV}`);
  console.log(`  js  moduleOf[${firstIdx}]      = ${jsV}`);
  // Show context: 16 entries around the divergence.
  const lo = Math.max(0, firstIdx - 8);
  const hi = Math.min(c.partition_end.length, firstIdx + 8);
  console.log(`  cpp [${lo}..${hi}]: ${c.partition_end.slice(lo, hi).join(", ")}`);
  console.log(`  js  [${lo}..${hi}]: ${j.moduleOf.slice(lo, hi).join(", ")}`);
}
process.exit(1);
