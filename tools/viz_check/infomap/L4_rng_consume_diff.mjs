/* L4 RNG-consumption diff: count getRandomizedIndexVector + uniformInt
 * draws JS makes per main-Infomap optimizeActiveNetwork call, vs the
 * cpp tracer's per-call (vo length + #non-empty linkOrders). If counts
 * diverge at call K, RNG state in JS goes out of sync with cpp at call K
 * + 1.
 *
 * Usage:
 *   node L4_rng_consume_diff.mjs <edge.csv> <seed> <tracer.json>
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 3) {
  console.error("usage: L4_rng_consume_diff.mjs <edge.csv> <seed> <tracer.json>");
  process.exit(2);
}
const [edgeCsv, seedArg, tracerJsonPath] = args;
const seed = parseInt(seedArg, 10) >>> 0;

globalThis.window = globalThis;
const WEB = path.join(__dirname, "../../../vltanh.github.io/comdet/js");
await import(path.join(WEB, "louvain/louvain.js"));
await import(path.join(WEB, "infomap/infomap_canon.js"));

// Load tracer JSON. The tracer prepends + appends [TRACE-IM] log lines.
let raw = fs.readFileSync(tracerJsonPath, "utf-8");
// Drop any leading non-JSON line
if (raw[0] !== "{") raw = raw.slice(raw.indexOf("\n") + 1);
// Drop any trailing non-JSON content after the matching closing brace.
const endBrace = raw.lastIndexOf("}");
const tracer = JSON.parse(raw.slice(0, endBrace + 1));

const mainCalls = tracer.calls.filter(c => tracer.levels[c.l].is_main);
const cppPerCall = mainCalls.map((c, i) => ({
  i, l: c.l, fl: !!c.fl, n_visits: c.visits.length,
  n_link_orders: c.visits.filter(v => v.lo.length > 0).length,
  // N candidate counts: each visit has lo.length = numModuleLinks.
  // randInt count for visit-order shuffle = N (active). For each
  // non-empty lo: lo.length randInts (Fisher-Yates over candidates).
  n_randints_estimate: c.visits.length
    + c.visits.reduce((acc, v) => acc + v.lo.length, 0),
}));

// Parse edge.csv as canonical_run.py does.
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

// Wrap LOUVAIN.MT19937 to count raw consumes per JS optimizeActiveNetwork
// invocation. Patch tryMoveEach to push (call_idx, raw_consumed_after_call)
// markers — easier: probe rng.raw counter before+after each
// runInfomapFaithful inner step.
const LV = globalThis.COMDET.LOUVAIN;
const realRng = LV.MT19937(seed);
let rawCount = 0;
const countingRng = {
  raw: () => { rawCount++; return realRng.raw(); },
  int: (lo, hi) => realRng.int(lo, hi),
  intLemire: (lo, hi) => realRng.intLemire(lo, hi),
  seed: (s) => realRng.seed(s),
};

// We can't easily intercept getRandomizedIndexVector internals without
// modifying infomap_canon. Instead, count raw uint32 consumes.
//
// Each getRandomizedIndexVector(rng, n) calls uniformInt(rng, 0, n-i-1)
// n times. Each uniformInt does 1+ raw() calls (Lemire rejection).
// So total raw count is APPROXIMATELY equal to N for each
// getRandomizedIndexVector. Not exact (rejections add 0 or more).

// Boundary log: capture rawCount at each tryMoveEach call boundary.
const jsBoundary = [];
function boundaryLog(label, info) {
  jsBoundary.push({ label, info, rawCount });
}

const t0 = Date.now();
const res = COMDET.INFOMAP_CANON.runInfomapFaithful(compactIds, edges, {
  seed: seed,
  rng: countingRng,
  aggregationLimit: 30,
  boundaryLog: boundaryLog,
});
const elapsed = (Date.now() - t0) / 1000;

// Sum cpp's expected raw consumption (visits + per-visit lo.length).
// Each randInt in cpp consumes 1+ raw() calls (Lemire rejection;
// expected ~1.0 per call). Approximate cpp consumption as visits +
// sum(lo.length).
const cppTotalRandInts = cppPerCall.reduce((a, c) =>
  a + c.n_randints_estimate, 0);

console.log(`seed=${seed}  cpp main calls: ${cppPerCall.length}`);
console.log(`  cpp expected randInt total: ${cppTotalRandInts}`);
console.log(`  JS rawCount (Lemire-debiased uniformInt + getRandomizedIndexVector internals): ${rawCount}`);
console.log(`  diff: ${rawCount - cppTotalRandInts}`);
console.log(`  JS finalL=${res.finalL.toFixed(15)}  cpp L_canon=${tracer.L_canon ? tracer.L_canon.toFixed(15) : "?"}`);
console.log("");
console.log("Per-cpp-call expected randInts:");
for (const c of cppPerCall) {
  console.log(`  call ${c.i}: level=${c.l} fl=${c.fl} visits=${c.n_visits} link_orders=${c.n_link_orders} ~randInts=${c.n_randints_estimate}`);
}

// JS tryMoveEach call boundaries — pair begin+end to measure draws/call.
console.log("");
console.log("JS tryMoveEach boundaries (rawCount delta = randInts consumed):");
const beginEvents = jsBoundary.filter(b => b.label === "tryMoveEach.begin");
const endEvents = jsBoundary.filter(b => b.label === "tryMoveEach.end");
let jsCallIdx = 0;
for (let k = 0; k < Math.min(beginEvents.length, endEvents.length); k++) {
  const b = beginEvents[k]; const e = endEvents[k];
  const draws = e.rawCount - b.rawCount;
  console.log(`  jsCall ${k}: fl=${b.info.fl} n=${b.info.n} draws=${draws}` +
    (k < cppPerCall.length ? `  cpp[${k}] ~randInts=${cppPerCall[k].n_randints_estimate} (level=${cppPerCall[k].l}, visits=${cppPerCall[k].n_visits})` : ""));
}
console.log(`  JS tryMoveEach calls: ${beginEvents.length}; cpp main calls: ${cppPerCall.length}`);

// Outer driver event sequence (compact view).
console.log("");
console.log("JS outer-driver event log (first 30 + last 5):");
const outerEvents = jsBoundary.filter(b => b.label !== "tryMoveEach.begin" && b.label !== "tryMoveEach.end");
for (let i = 0; i < Math.min(30, outerEvents.length); i++) {
  const ev = outerEvents[i];
  console.log(`  [${ev.rawCount.toString().padStart(5)}] ${ev.label} ${JSON.stringify(ev.info)}`);
}
if (outerEvents.length > 30) {
  console.log(`  ... ${outerEvents.length - 30} more ...`);
  for (const ev of outerEvents.slice(-5)) {
    console.log(`  [${ev.rawCount.toString().padStart(5)}] ${ev.label} ${JSON.stringify(ev.info)}`);
  }
}
