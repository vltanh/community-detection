/* L4 RNG-state diff: at every JS tryMoveEach boundary, peek next 5 raw
 * uint32 outputs from the JS rng + compare against the cpp tracer's
 * `rng` field. The first call where peeks diverge localises the L4
 * self-RNG divergence to a single sub-call boundary.
 *
 * Pairs cpp main calls (filtered via tracer.calls.filter is_main) with
 * the corresponding leading JS tryMoveEach calls (the JS sub-Infomap
 * recursion produces non-main calls in between, so JS jsCalls are the
 * superset; main-call indices map via outer-event log).
 *
 * Usage:
 *   node L4_rng_state_diff.mjs <edge.csv> <seed> <tracer.json>
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 3) {
  console.error("usage: L4_rng_state_diff.mjs <edge.csv> <seed> <tracer.json>");
  process.exit(2);
}
const [edgeCsv, seedArg, tracerJsonPath] = args;
const seed = parseInt(seedArg, 10) >>> 0;

globalThis.window = globalThis;
const WEB = path.join(__dirname, "../../../vltanh.github.io/comdet/js");
await import(path.join(WEB, "louvain/louvain.js"));
await import(path.join(WEB, "infomap/infomap_canon.js"));

let raw = fs.readFileSync(tracerJsonPath, "utf-8");
if (raw[0] !== "{") raw = raw.slice(raw.indexOf("\n") + 1);
const endBrace = raw.lastIndexOf("}");
const tracer = JSON.parse(raw.slice(0, endBrace + 1));

// Parse edge.csv (canonical_run.py convention).
const lines = fs.readFileSync(edgeCsv, "utf-8").trim().split(/\r?\n/);
const dataLines = lines.slice(1);
const srcs = [], tgts = [];
for (const ln of dataLines) {
  const [s, t] = ln.split(",");
  srcs.push(s); tgts.push(t);
}
const seen = new Map();
const nodes = [];
for (const s of srcs) if (!seen.has(s)) { seen.set(s, nodes.length); nodes.push(s); }
for (const t of tgts) if (!seen.has(t)) { seen.set(t, nodes.length); nodes.push(t); }
const edges = srcs.map((s, i) => [seen.get(s), seen.get(tgts[i])]);
const compactIds = nodes.map((_, i) => i);

// Run JS port with boundary log capturing rng.peek(5) at each tryMoveEach entry.
const LV = globalThis.COMDET.LOUVAIN;
const rng = LV.MT19937(seed);
const jsBoundary = [];
function boundaryLog(label, info, currentRng) {
  if (label === "tryMoveEach.begin") {
    // Peek the rng AT THIS SCOPE (parent or sub-Infomap's). Sub-Infomap
    // creates its own rng, so the diagnostic must peek that one to
    // mirror cpp's m_rand peek inside the sub-Infomap.
    const r = currentRng || rng;
    jsBoundary.push({ idx: jsBoundary.length, info, peek: r.peek(5) });
  }
}
COMDET.INFOMAP_CANON.runInfomapFaithful(compactIds, edges, {
  seed: seed,
  rng: rng,
  aggregationLimit: 30,
  boundaryLog: boundaryLog,
});

// cpp main calls. Note: tracer.calls includes both is_main and non-main calls.
const cppCalls = tracer.calls;
const cppMain = cppCalls.filter(c => tracer.levels[c.l].is_main);

// Compare: pair JS jsCall k with cpp call index k SIMPLY (same call order).
// JS includes sub-Infomap calls; cpp tracer also includes sub-Infomap calls
// (just is_main=false). Pair against the FULL cpp call list.
console.log(`seed=${seed}  cpp total calls=${cppCalls.length}  JS calls=${jsBoundary.length}`);
console.log("");
console.log("Per-call rng peek diff (first divergence):");
let firstDiv = -1;
const N = Math.min(cppCalls.length, jsBoundary.length);
for (let k = 0; k < N; k++) {
  const cpp = cppCalls[k];
  const js = jsBoundary[k];
  const cppPeek = cpp.rng || [];
  const jsPeek = js.peek;
  let match = cppPeek.length === jsPeek.length;
  for (let i = 0; i < cppPeek.length && match; i++) {
    if ((cppPeek[i] >>> 0) !== (jsPeek[i] >>> 0)) match = false;
  }
  if (!match) {
    console.log(`  call ${k}: DIVERGE`);
    console.log(`    cpp peek: ${cppPeek.join(",")}`);
    console.log(`    js  peek: ${jsPeek.join(",")}`);
    console.log(`    cpp call meta: l=${cpp.l} fl=${cpp.fl} is_main=${tracer.levels[cpp.l].is_main} active_n=${tracer.levels[cpp.l].active_n}`);
    if (firstDiv < 0) firstDiv = k;
    if (k - firstDiv > 3) break;
  } else if (k < 5 || k % 10 === 0) {
    console.log(`  call ${k}: match (${cppPeek[0]})`);
  }
}
if (firstDiv < 0) {
  console.log("  ALL " + N + " call boundaries match (rng state in lockstep).");
} else {
  console.log(`  first divergence at call ${firstDiv}.`);
}
console.log(`  cpp calls=${cppCalls.length}  js calls=${jsBoundary.length}  diff=${jsBoundary.length - cppCalls.length}`);
