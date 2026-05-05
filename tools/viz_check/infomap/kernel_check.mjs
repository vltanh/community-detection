/* Infomap JS-vs-tracer byte-equal cross-check.
 *
 * Reads the tracer JSON produced by /tmp/infomap_kernel_check on the
 * canonical edge file and verifies:
 *   (1) JS map-equation on the FINAL stage's leaf_to_top reproduces
 *       the tracer's reported L within 1e-9 (formula byte-equal at
 *       machine epsilon). Intermediate stages are skipped because
 *       canonical's getCodelength returns the SUPER-network codelength
 *       mid-loop, not the leaf-flat L of leaf_to_top.
 *   (2) JS map-equation on the canonical com.csv partition matches
 *       L_canon within 1e-9 (independent formula check).
 *   (3) The JS-canonicalized final partition (drop singletons + ASC
 *       renumber + sort by node_id) matches the canonical com.csv
 *       exactly.
 *
 * Three-leg byte-equal claim: canonical pypi com.csv == tracer com.csv
 * (verified by kernel_check.py diff) AND L_canon == L_js at machine eps.
 *
 * Usage:
 *   node kernel_check.mjs <tracer.json> <canonical.csv>
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 2) {
  console.error("usage: kernel_check.mjs <tracer.json> <canonical.csv>");
  process.exit(2);
}
const [tracerJsonPath, canonCsvPath] = args;

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = path.join(__dirname, "../../../vltanh.github.io/comdet/js");
await import(path.join(WEB, "louvain/louvain.js"));
await import(path.join(WEB, "infomap/infomap.js"));

function loadTracer(p) {
  return JSON.parse(fs.readFileSync(p, "utf8"));
}

function loadCanonCsv(p) {
  const lines = fs.readFileSync(p, "utf8").trim().split(/\r?\n/);
  const m = new Map();
  for (let i = 1; i < lines.length; i++) {
    const cols = lines[i].split(",");
    m.set(parseInt(cols[0], 10), parseInt(cols[1], 10));
  }
  return m;
}

const tracer = loadTracer(tracerJsonPath);
const canonMem = loadCanonCsv(canonCsvPath);

// Build the JS graph using the SAME compact-id mapping the tracer used
// (renum_to_orig: compact_idx -> orig_id). This guarantees the JS
// stationary distribution / mapEquation receive identical structure.
const renumToOrig = tracer.renum_to_orig.map(x => +x);
const n = renumToOrig.length;
// Build edges in compact-id space.
const edges = tracer.edges; // array of [u_compact, v_compact]
// JS COMDET.INFOMAP.buildGraph expects an array of arbitrary node ids.
// Use 0..n-1 as ids and feed the raw compact edges directly.
const compactIds = [];
for (let i = 0; i < n; i++) compactIds.push(i);
const g = COMDET.INFOMAP.buildGraph(compactIds, edges);
const p = COMDET.INFOMAP.stationary(g);

let failures = 0;
const eps = 1e-9;

function checkL(label, partition, L_expected) {
  const L_js = COMDET.INFOMAP.mapEquation(g, p, partition).L;
  const dL = Math.abs(L_js - L_expected);
  const status = dL < eps ? "PASS" : "FAIL";
  if (status === "FAIL") failures += 1;
  return { L_js, L_expected, dL, status };
}

console.log("=== Infomap 3-leg byte-equal cross-check ===");
console.log(`n=${n}  m=${edges.length}  L_canon=${tracer.L_canon}`);
console.log(`tracer reported ${tracer.stages.length} stages`);

const finalStage = tracer.stages[tracer.stages.length - 1];

// (1) Final-stage check — leaf_to_top is the leaf-flat partition only
// at the final stage; intermediate stages reflect super-network state
// inside the optimizer, where getCodelength is computed at a higher
// aggregation level and isn't comparable to a leaf-flat mapEq.
if (finalStage.leaf_to_top.length === n) {
  const r = checkL(finalStage.label, finalStage.leaf_to_top, finalStage.L);
  console.log(`final stage L_js=${r.L_js.toFixed(12)}  L_trace=${r.L_expected.toFixed(12)}  Δ=${r.dL.toExponential(3)}  ${r.status}`);
} else {
  console.log("final stage leaf_to_top length mismatch — SKIP");
  failures += 1;
}

// (2) JS L on canonical com.csv partition vs L_canon. Singleton nodes
// (those dropped from canonical's post-processed CSV) get unique
// cluster ids offset above the kept-cluster range so they don't merge
// into kept clusters during the JS mapEq evaluation.
const canonPart = new Array(n);
const canonRemap = new Map();
let nextSing = 0;
const startSing = canonMem.size + n;
for (let i = 0; i < n; i++) {
  const orig = renumToOrig[i];
  if (canonMem.has(orig)) {
    const c = canonMem.get(orig);
    if (!canonRemap.has(c)) canonRemap.set(c, canonRemap.size);
    canonPart[i] = canonRemap.get(c);
  } else {
    canonPart[i] = startSing + (nextSing++);
  }
}
const r1 = checkL("L_canon", canonPart, tracer.L_canon);
console.log(`L_js on canonical partition = ${r1.L_js.toFixed(12)}  L_canon = ${r1.L_expected.toFixed(12)}  Δ = ${r1.dL.toExponential(3)}  ${r1.status}`);

// (3) JS-canonicalized final partition (drop singletons + ASC renumber +
// sort by node_id) compared against canonical com.csv membership.
let mismatch_partition = 0;
if (finalStage.leaf_to_top.length === n) {
  // Canonicalize finalStage.leaf_to_top (drop singletons, renumber,
  // sort by orig_id) and diff against canonical com.csv.
  const counts = new Map();
  for (const c of finalStage.leaf_to_top) {
    counts.set(c, (counts.get(c) || 0) + 1);
  }
  const kept = [];
  for (let i = 0; i < n; i++) {
    if (counts.get(finalStage.leaf_to_top[i]) > 1) {
      kept.push([renumToOrig[i], finalStage.leaf_to_top[i]]);
    }
  }
  const ucs = Array.from(new Set(kept.map(r => r[1]))).sort((a, b) => a - b);
  const remap = new Map();
  ucs.forEach((c, i) => remap.set(c, i));
  const renum = kept.map(r => [r[0], remap.get(r[1])]).sort((a, b) => a[0] - b[0]);
  const tracerMem = new Map();
  for (const [orig, c] of renum) tracerMem.set(orig, c);
  // Compare canonMem vs tracerMem; require identical sets and per-node match.
  if (canonMem.size !== tracerMem.size) {
    console.log(`partition size mismatch: canon=${canonMem.size} tracer=${tracerMem.size}`);
    mismatch_partition += 1;
  } else {
    for (const [orig, c] of canonMem) {
      if (tracerMem.get(orig) !== c) { mismatch_partition += 1; break; }
    }
  }
}
if (mismatch_partition === 0) {
  console.log(`final partition canonical == tracer (post-processed): PASS (${canonMem.size} kept rows)`);
} else {
  console.log(`final partition canonical != tracer: FAIL`);
  failures += 1;
}

if (failures) {
  console.log(`OVERALL: FAIL (${failures})`);
  process.exit(1);
}
console.log(`OVERALL: PASS`);
process.exit(0);
