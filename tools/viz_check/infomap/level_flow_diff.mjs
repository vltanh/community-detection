/* Per-level super-vertex flow diff: cpp tracer vs JS faithful kernel.
 *
 * Captures the per-vertex flow / enterFlow / exitFlow arrays at every
 * `initPartition` (cpp `traceBeginLevel`, JS `__INFOMAP_LEVEL_DUMP`)
 * and walks them lockstep to find the FIRST level where any super-
 * vertex value differs by >0 ulp. This isolates whether residual
 * trajectory divergence (under matching seed) originates at level
 * entry (collapseGraph / aggregateFlow drift) or per-move (m_module
 * FlowData accumulator drift inside the prior sweep).
 *
 * Usage:
 *   node level_flow_diff.mjs <cpp_tracer.json> <edge.csv> <seed>
 *
 * Side-by-side with visit_decision_diff.mjs which compares per-visit
 * candidates + tie-break + pair-pull. This harness fires earlier in
 * the trajectory (level boundary precedes the visits at that level).
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 3) {
  console.error("usage: level_flow_diff.mjs <cpp_tracer.json> <edge.csv> <seed>");
  process.exit(2);
}
const [tracerPath, edgeCsv, seedArg] = args;
const seed = parseInt(seedArg, 10) >>> 0;

let raw = fs.readFileSync(tracerPath, "utf-8");
if (raw[0] !== "{") raw = raw.slice(raw.indexOf("\n") + 1);
raw = raw.slice(0, raw.lastIndexOf("}") + 1);
const cpp = JSON.parse(raw);

const cppLevels = cpp.levels || [];
console.log(`cpp levels captured: ${cppLevels.length}`);

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = path.join(__dirname, "../../../vltanh.github.io/comdet/js");
await import(path.join(WEB, "louvain/louvain.js"));
await import(path.join(WEB, "infomap/infomap_canon.js"));

const jsLevels = [];
globalThis.__INFOMAP_LEVEL_DUMP = function(rec) {
  jsLevels.push({
    n: rec.n,
    flow: rec.flow.slice(),
    enter: rec.enter.slice(),
    exit: rec.exit.slice(),
    leafToActive: rec.leafToActive ? rec.leafToActive.slice() : null,
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
console.log(`js  levels captured: ${jsLevels.length} (in ${elapsed.toFixed(2)}s)`);
console.log(`js  finalL=${res.finalL.toFixed(15)}  cpp finalL=${cpp.L_canon.toFixed(15)}`);

// Walk lockstep.
function bits(x) { const b = new Float64Array(1); b[0] = x; return new BigUint64Array(b.buffer)[0]; }
function bitDiff(a, b) { return bits(a) !== bits(b); }

const N = Math.min(cppLevels.length, jsLevels.length);
console.log(`comparing ${N} levels (cpp=${cppLevels.length} js=${jsLevels.length})`);

let firstLvl = -1, firstField = null, firstIdx = -1;
let cppV = 0, jsV = 0;
const sumLen = (a) => a.reduce((x, y) => x + y, 0);
let cumPerLevelOk = 0;
for (let l = 0; l < N; l++) {
  const c = cppLevels[l];
  const j = jsLevels[l];
  if (c.active_n !== j.n) {
    console.log(`level ${l}: SIZE MISMATCH cpp=${c.active_n} js=${j.n}`);
    if (firstLvl < 0) { firstLvl = l; firstField = "active_n"; firstIdx = -1; cppV = c.active_n; jsV = j.n; }
    continue;
  }
  for (const f of ["flow", "enter", "exit"]) {
    const cppArr = (f === "flow") ? c.init_flow : (f === "enter") ? c.init_enter : c.init_exit;
    const jsArr  = (f === "flow") ? j.flow      : (f === "enter") ? j.enter      : j.exit;
    if (!cppArr || !jsArr) continue;
    for (let i = 0; i < c.active_n; i++) {
      if (bitDiff(cppArr[i], jsArr[i])) {
        if (firstLvl < 0) {
          firstLvl = l; firstField = `init_${f}`; firstIdx = i;
          cppV = cppArr[i]; jsV = jsArr[i];
        }
      }
    }
  }
  if (firstLvl < 0) cumPerLevelOk++;
}

// Find first level where leaf_to_active diverges (identity check —
// orthogonal to flow values; if super-vertex ordering swaps but
// happens to give same per-slot flow, this catches it).
let firstMembLvl = -1, firstMembIdx = -1, firstMembCpp = -1, firstMembJs = -1;
for (let l = 0; l < N; l++) {
  const c = cppLevels[l]; const j = jsLevels[l];
  if (!c.leaf_to_active || !j.leafToActive) continue;
  const m = Math.min(c.leaf_to_active.length, j.leafToActive.length);
  for (let i = 0; i < m; i++) {
    if (c.leaf_to_active[i] !== j.leafToActive[i]) {
      firstMembLvl = l; firstMembIdx = i;
      firstMembCpp = c.leaf_to_active[i]; firstMembJs = j.leafToActive[i];
      break;
    }
  }
  if (firstMembLvl >= 0) break;
}
if (firstMembLvl >= 0) {
  console.log(`\nLEAF-TO-ACTIVE DIVERGENCE: first at level=${firstMembLvl} leaf_idx=${firstMembIdx} cpp=${firstMembCpp} js=${firstMembJs}`);
} else if (cppLevels.some(l => l.leaf_to_active && l.leaf_to_active.length > 0)
        && jsLevels.some(l => l.leafToActive && l.leafToActive.length > 0)) {
  console.log(`\nleaf_to_active byte-equal where both present`);
}

if (firstLvl < 0) {
  console.log(`ALL ${N} levels byte-equal across init_flow / init_enter / init_exit`);
  process.exit(0);
}

const c = cppLevels[firstLvl];
const j = jsLevels[firstLvl];
console.log(`\nFIRST DIVERGENCE at level=${firstLvl} field=${firstField} idx=${firstIdx}`);
console.log(`  cpp: is_main=${c.is_main} active_n=${c.active_n} init_L=${c.init_L} init_L_index=${c.init_L_index} init_L_module=${c.init_L_module}`);
console.log(`  js : n=${j.n}`);
console.log(`  cpp ${firstField}[${firstIdx}] = ${cppV}`);
console.log(`  js  ${firstField}[${firstIdx}] = ${jsV}`);
console.log(`  Δ = ${(cppV - jsV).toExponential(3)}, bit-Δ = ${(bits(cppV) ^ bits(jsV)).toString(16)}`);
console.log(`prior levels OK: ${cumPerLevelOk}`);

// Show head of arrays for context.
const showN = Math.min(c.active_n, 16);
const cArr = (firstField === "init_flow") ? c.init_flow : (firstField === "init_enter") ? c.init_enter : c.init_exit;
const jArr = (firstField === "init_flow") ? j.flow      : (firstField === "init_enter") ? j.enter      : j.exit;
console.log(`\ncpp ${firstField}[0..${showN}]: ${cArr.slice(0, showN).map(x => x.toExponential(6)).join(", ")}`);
console.log(`js  ${firstField}[0..${showN}]: ${jArr.slice(0, showN).map(x => x.toExponential(6)).join(", ")}`);
process.exit(1);
