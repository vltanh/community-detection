/* Per-visit decision diff: cpp tracer vs JS faithful kernel.
 *
 * Captures EVERY tryMoveEachNodeIntoBestModule visit's decision data
 * (candidate set + best/strongest pick + tie-break flag + pair-pull
 * state) from both cpp + JS, then walks them in lockstep and prints
 * the first divergent visit.
 *
 * Useful for residual-fail cells where the partition diverges (cpp
 * merges 1 module that JS doesn't, or vice versa) despite matching
 * codelengths — pinpoints WHICH visit, WHICH candidate, WHICH branch.
 *
 * Usage:
 *   node visit_decision_diff.mjs <cpp_tracer.json> <edge.csv> <seed>
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 3) {
  console.error("usage: visit_decision_diff.mjs <cpp_tracer.json> <edge.csv> <seed>");
  process.exit(2);
}
const [tracerPath, edgeCsv, seedArg] = args;
const seed = parseInt(seedArg, 10) >>> 0;

// Parse cpp trace.
let raw = fs.readFileSync(tracerPath, "utf-8");
if (raw[0] !== "{") raw = raw.slice(raw.indexOf("\n") + 1);
raw = raw.slice(0, raw.lastIndexOf("}") + 1);
const cpp = JSON.parse(raw);

// Flatten cpp visits in (call_idx, visit_idx) order.
const cppVisits = [];
for (let ci = 0; ci < cpp.calls.length; ci++) {
  const call = cpp.calls[ci];
  for (let vi = 0; vi < call.visits.length; vi++) {
    const v = call.visits[vi];
    cppVisits.push({
      ci, vi, lvl: call.l, fl: call.fl,
      v: v.v, m: v.m, n: v.n,
      cand: v.cand, candDE: v.candDE, candDX: v.candDX, candDL: v.candDL,
      bM: v.bM, bDL: v.bDL, sM: v.sM, sDL: v.sDL, sPick: v.sPick,
      nLnk: v.nLnk, ppV: v.ppV, ppOM: v.ppOM, ppT: v.ppT,
      L: v.L,
    });
  }
}
console.log(`cpp visits captured: ${cppVisits.length} across ${cpp.calls.length} calls`);

// Bootstrap JS env + capture visit decisions via global hook.
globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = path.join(__dirname, "../../../vltanh.github.io/comdet/js");
await import(path.join(WEB, "louvain/louvain.js"));
await import(path.join(WEB, "infomap/infomap_canon.js"));

// Strategy: every onVisit pushes a record (skipped or evaluated).
// Decision callback fires AFTER onVisit only for evaluated visits;
// it fills in candidate fields on the LAST jsVisits entry.
const jsVisits = [];
let curCallIdx = -1;
globalThis.__INFOMAP_CALL_BEGIN = function(/*n*/) { curCallIdx += 1; };
globalThis.__INFOMAP_ONVISIT = function(v, moved, newM, L, Li, Lm, ef, ele, xle, fle, nfle) {
  jsVisits.push({
    ci: curCallIdx, vi: jsVisits.length,
    v: v, m: moved ? 1 : 0, n: newM, L: L,
    Li, Lm, ef, ele, xle, fle, nfle,
    cand: [], candDE: [], candDX: [], candDL: [],
    bM: 0, bDL: 0, sM: 0, sDL: 0, sPick: 0,
    nLnk: 0, ppV: -1, ppOM: 0, ppT: 0,
    decided: false,
  });
};
globalThis.__INFOMAP_VISIT_DECISION = function(v, candM, candDE, candDX, candDL,
    bM, bDL, sM, sDL, sPick,
    nLnk, ppV, ppOM, ppT) {
  if (jsVisits.length === 0) return;
  const last = jsVisits[jsVisits.length - 1];
  if (last.v !== v) {
    // Out-of-order: shouldn't happen since decision fires post-onVisit
    // for the same visit.
    console.error(`decision/onVisit mismatch: last.v=${last.v} decision.v=${v}`);
  }
  last.cand = Array.isArray(candM) ? candM.slice() : Array.from(candM);
  last.candDE = Array.isArray(candDE) ? candDE.slice() : Array.from(candDE);
  last.candDX = Array.isArray(candDX) ? candDX.slice() : Array.from(candDX);
  last.candDL = Array.isArray(candDL) ? candDL.slice() : Array.from(candDL);
  last.bM = bM; last.bDL = bDL;
  last.sM = sM; last.sDL = sDL;
  last.sPick = sPick ? 1 : 0;
  last.nLnk = nLnk;
  last.ppV = ppV;
  last.ppOM = ppOM;
  last.ppT = ppT ? 1 : 0;
  last.decided = true;
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
for (let i = 0; i < srcs.length; i++) {
  edges.push([seen.get(srcs[i]), seen.get(tgts[i])]);
}
const compactIds = nodes.map((_, i) => i);

const t0 = Date.now();
const res = COMDET.INFOMAP_CANON.runInfomapFaithful(compactIds, edges, {
  seed: seed,
  aggregationLimit: 30,
});
const elapsed = (Date.now() - t0) / 1000;

console.log(`js  visits captured: ${jsVisits.length} (in ${elapsed.toFixed(2)}s)`);
console.log(`js  finalL=${res.finalL.toFixed(15)}  cpp finalL=${cpp.L_canon.toFixed(15)}`);

// Walk lockstep, print first divergence.
const N = Math.min(cppVisits.length, jsVisits.length);
// Find first ΔL_after, ΔenterFlow, etc. divergence in temporal order.
// cpp + js visits use the same field keys (no rename), so direct lookup.
const accFields = ['L', 'ef', 'ele', 'xle', 'fle', 'nfle', 'eflnef', 'exnf', 'exfle'];
const seenFirst = {};
for (let k = 0; k < N; k++) {
  const c = cppVisits[k];
  const j = jsVisits[k];
  for (const f of accFields) {
    if (seenFirst[f]) continue;
    const cv = c[f];
    const jv = j[f];
    if (cv == null || jv == null) continue;
    if (cv !== jv) {
      seenFirst[f] = true;
      console.log(`first Δ${f} at flat-idx=${k}: cpp=${cv} js=${jv} Δ=${(cv - jv).toExponential(3)}`);
      console.log(`  cpp call=${c.ci} v=${c.v} m=${c.m} n=${c.n}  | js call=${j.ci} v=${j.v} m=${j.m} n=${j.n}`);
    }
  }
}
let firstDiv = -1, divField = null;
function arrEq(a, b, tol) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) {
    if (tol > 0) { if (Math.abs(a[i] - b[i]) > tol) return false; }
    else { if (a[i] !== b[i]) return false; }
  }
  return true;
}
for (let k = 0; k < N; k++) {
  const c = cppVisits[k];
  const j = jsVisits[k];
  if (c.v !== j.v) { firstDiv = k; divField = "v"; break; }
  if (c.m !== j.m) { firstDiv = k; divField = "moved"; break; }
  if (c.m === 1 && c.n !== j.n) { firstDiv = k; divField = "newM"; break; }
  if (!arrEq(c.cand, j.cand, 0)) { firstDiv = k; divField = "cand_modules"; break; }
  if (!arrEq(c.candDE, j.candDE, 1e-12)) { firstDiv = k; divField = "candDE"; break; }
  if (!arrEq(c.candDX, j.candDX, 1e-12)) { firstDiv = k; divField = "candDX"; break; }
  if (!arrEq(c.candDL, j.candDL, 1e-9)) { firstDiv = k; divField = "candDL"; break; }
  if (c.bM !== j.bM) { firstDiv = k; divField = "bestM"; break; }
  if (c.sM !== j.sM) { firstDiv = k; divField = "strongM"; break; }
  if (c.sPick !== j.sPick) { firstDiv = k; divField = "strongPicked"; break; }
  if (c.nLnk !== j.nLnk) { firstDiv = k; divField = "numLinkedInOld"; break; }
  if (c.ppV !== j.ppV) { firstDiv = k; divField = "pairPullV"; break; }
  if (c.ppT !== j.ppT) { firstDiv = k; divField = "pairPullTriggered"; break; }
}
if (firstDiv < 0) {
  console.log(`OVERALL: ${cppVisits.length === jsVisits.length ? "all visits decision-byte-equal" : "EXHAUSTED short side; cpp=" + cppVisits.length + " js=" + jsVisits.length}`);
  if (cppVisits.length !== jsVisits.length) process.exit(1);
  process.exit(0);
}
const c = cppVisits[firstDiv];
const j = jsVisits[firstDiv];
console.log(`FIRST DIVERGENCE at flat-idx=${firstDiv}, field='${divField}'`);
console.log(`  cpp: call=${c.ci} visit=${c.vi} lvl=${c.lvl} fl=${c.fl} v=${c.v} m=${c.m} n=${c.n} L=${c.L}`);
console.log(`  js : call=${j.ci} visit=${j.vi}                  v=${j.v} m=${j.m} n=${j.n} L=${j.L}`);
console.log(`  cpp cand:   ${JSON.stringify(c.cand)}`);
console.log(`  js  cand:   ${JSON.stringify(j.cand)}`);
console.log(`  cpp candDE: [${c.candDE.map(x => x.toExponential(6)).join(", ")}]`);
console.log(`  js  candDE: [${j.candDE.map(x => x.toExponential(6)).join(", ")}]`);
console.log(`  cpp candDX: [${c.candDX.map(x => x.toExponential(6)).join(", ")}]`);
console.log(`  js  candDX: [${j.candDX.map(x => x.toExponential(6)).join(", ")}]`);
console.log(`  cpp candDL: [${c.candDL.map(x => x.toExponential(6)).join(", ")}]`);
console.log(`  js  candDL: [${j.candDL.map(x => x.toExponential(6)).join(", ")}]`);
console.log(`  cpp bM=${c.bM} bDL=${c.bDL} sM=${c.sM} sDL=${c.sDL} sPick=${c.sPick} nLnk=${c.nLnk} ppV=${c.ppV} ppT=${c.ppT}`);
console.log(`  js  bM=${j.bM} bDL=${j.bDL} sM=${j.sM} sDL=${j.sDL} sPick=${j.sPick} nLnk=${j.nLnk} ppV=${j.ppV} ppT=${j.ppT}`);

// Show last 3 visits of context.
console.log("\nlast 3 visits before divergence:");
for (let k = Math.max(0, firstDiv - 3); k < firstDiv; k++) {
  const cc = cppVisits[k];
  const jj = jsVisits[k];
  console.log(`  [${k}] cpp call=${cc.ci} v=${cc.v} m=${cc.m} n=${cc.n} | js call=${jj.ci} v=${jj.v} m=${jj.m} n=${jj.n}`);
}
process.exit(1);
