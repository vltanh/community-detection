/* IKC kernel cross-check, JS replay leg.
 *
 * Reads <pyJson> (output of instrumented/kernel_check.py) and runs
 * COMDET.IKC.runIKC against the same input. IKC is deterministic
 * (no RNG), so JS replay needs no oracle injection — just match
 * per-iteration (max_k, components, kept) and final partition.
 *
 * Acceptance:
 *   - per-iter max_k matches
 *   - per-iter component sizes match (sorted ASC)
 *   - per-iter kept-component sizes match (sorted ASC)
 *   - final partition CSV byte-equal vs canonical (when sorted +
 *     header-stripped + cluster-id renumbered)
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 3) {
  console.error("usage: kernel_check.mjs <pyJson> <edge.csv> <out.csv>");
  process.exit(2);
}
const [pyPath, edgePath, outCsv] = args;

const py = JSON.parse(fs.readFileSync(pyPath, "utf8"));
const kFloor = py.k_floor;

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = path.join(__dirname, "../../../vltanh.github.io/comdet/js");
await import(path.join(WEB, "ikc/ikc.js"));

// Load edges from CSV (positional first two columns; skip self-loops to
// match canonical run_ikc.format_graph's removeSelfLoops + JS runIKC's
// e[0]===e[1] guard).
function loadEdges(edgePath) {
  const lines = fs.readFileSync(edgePath, "utf8").trim().split(/\r?\n/);
  const nodeSet = new Set();
  const edges = [];
  for (let i = 1; i < lines.length; i++) {
    if (!lines[i]) continue;
    const cols = lines[i].split(/[,\t ]/);
    const u = parseInt(cols[0], 10);
    const v = parseInt(cols[1], 10);
    if (u === v) continue;
    nodeSet.add(u);
    nodeSet.add(v);
    edges.push([u, v]);
  }
  // Encounter order to match canonical's pd.unique.
  const seen = new Set();
  const nodes = [];
  for (let i = 1; i < lines.length; i++) {
    if (!lines[i]) continue;
    const cols = lines[i].split(/[,\t ]/);
    const u = parseInt(cols[0], 10);
    const v = parseInt(cols[1], 10);
    if (!seen.has(u)) { seen.add(u); nodes.push(u); }
    if (!seen.has(v)) { seen.add(v); nodes.push(v); }
  }
  return { nodes, edges };
}

const { nodes, edges } = loadEdges(edgePath);
const res = COMDET.IKC.runIKC(nodes, edges, { kFloor, canonicalGate: true });

// Compare per-iteration with python trace.
const pyIters = py.iters;
const jsIters = res.iterations;

let fail = false;
function fmt(arr) { return JSON.stringify(arr); }

// Python iter records have {iter, max_k, components:[{size,kept,k_valid,modular_pos}], kept_clusters_this_iter}
// or {iter, max_k, terminated} on bail. Trim JS to comparable shape.
const pyActive = pyIters.filter(p => !p.terminated);
const jsActive = jsIters.filter(j => !j.bailed);

if (pyActive.length !== jsActive.length) {
  console.error(`ITER_COUNT mismatch: py=${pyActive.length} js=${jsActive.length}`);
  fail = true;
}

const lim = Math.min(pyActive.length, jsActive.length);
for (let i = 0; i < lim; i++) {
  const p = pyActive[i];
  const j = jsActive[i];
  if (p.max_k !== j.maxK) {
    console.error(`iter=${i} max_k mismatch py=${p.max_k} js=${j.maxK}`);
    fail = true;
  }
  const pSizes = p.components.map(c => c.size).sort((a,b)=>a-b);
  const jSizes = j.components.map(c => c.nodes.length).sort((a,b)=>a-b);
  if (fmt(pSizes) !== fmt(jSizes)) {
    console.error(`iter=${i} comp sizes py=${fmt(pSizes)} js=${fmt(jSizes)}`);
    fail = true;
  }
  const pKept = p.components.filter(c => c.kept).map(c => c.size).sort((a,b)=>a-b);
  const jKept = j.components.filter(c => c.accepted).map(c => c.nodes.length).sort((a,b)=>a-b);
  if (fmt(pKept) !== fmt(jKept)) {
    console.error(`iter=${i} kept sizes py=${fmt(pKept)} js=${fmt(jKept)}`);
    fail = true;
  }
}

// Bail-iter alignment.
const pBail = pyIters.find(p => p.terminated);
const jBail = jsIters.find(j => j.bailed);
if (!!pBail !== !!jBail) {
  console.error(`bail mismatch: py=${!!pBail} js=${!!jBail}`);
  fail = true;
} else if (pBail && jBail) {
  if (pBail.max_k !== jBail.maxK) {
    console.error(`bail max_k mismatch: py=${pBail.max_k} js=${jBail.maxK}`);
    fail = true;
  }
}

// Write CSV in run_ikc.print_clusters format. Accepted-cluster ordering
// in JS comes from JS's connectedComponentsCompact iteration; canonical
// comes from NetworKit's WeaklyConnectedComponents.getComponents() over
// a re-compacted graph each iteration. Order can diverge while membership
// is identical. Bridge: match each tracer-canonical cluster (size > 1,
// in tracer order) to a JS accepted cluster by member-set equality, and
// emit CSV using that order. Verifies partition equality and byte-equal
// vs canonical CSV simultaneously.
const tracerOrder = py.final_clusters.filter(c => c.size > 1);
const jsKept = res.accepted.filter(cl => cl.members.length > 1);

function memberKey(arr) { return JSON.stringify(arr.slice().sort((a,b)=>a-b)); }
const jsByKey = new Map();
jsKept.forEach(cl => jsByKey.set(memberKey(cl.members), cl));

if (tracerOrder.length !== jsKept.length) {
  console.error(`KEPT_COUNT mismatch tracer=${tracerOrder.length} js=${jsKept.length}`);
  fail = true;
}

const lines2 = ["node_id,cluster_id"];
let cid = 0;
for (const tc of tracerOrder) {
  // tracer cluster nodes are compact-internal IDs (post format_graph,
  // which is identity here since g1 was already 0..n-1). Map each via
  // the encounter-order node_map back to orig name.
  const origNodes = tc.nodes.map(ci => parseInt(py.node_map[ci].orig, 10));
  const key = memberKey(origNodes);
  const m = jsByKey.get(key);
  if (!m) {
    console.error(`tracer cluster ${cid} (orig=${origNodes.slice(0,5)}...) has no JS match`);
    fail = true;
    cid += 1;
    continue;
  }
  // Emit node-rows in tracer (canonical) order for byte-equal CSV.
  origNodes.forEach(node => lines2.push(`${node},${cid}`));
  cid += 1;
}
fs.writeFileSync(outCsv, lines2.join("\n") + "\n");

if (fail) {
  console.error("FAIL");
  process.exit(1);
} else {
  console.error(`PASS iter_compare ${pyActive.length} active iters`);
}
