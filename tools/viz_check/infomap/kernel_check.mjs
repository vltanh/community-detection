/* Infomap random-oracle deterministic-core verification.
 *
 * Phase 1: validate JS optimizer's deterministic decisions on the very
 * first call to tryMoveEachNodeIntoBestModule (= leaf-level, first
 * sweep, first findTopModulesRepeatedly iteration). The tracer dumps
 * canonical's:
 *   - level-0 active-network seed (per-leaf flow/enter/exit)
 *   - call[0].vo: post-randomization visit_order
 *   - per-visit lo: post-randomization module-link order (when canonical
 *     reached the link-randomization step; empty for visits skipped by
 *     the dirty bit or first-loop guard)
 *   - per-visit (moved, newM, L_after): canonical's deterministic
 *     decision given that random sequence
 *
 * JS replay constructs the same active-graph from the seed, calls
 * COMDET.INFOMAP_CANON.tryMoveEach with the visit_order + linkOrders
 * oracle injected, and per-visit emits its OWN (moved, newM, L_after).
 * The two streams must agree per visit:
 *   - (moved, newM) byte-equal — validates JS's diffMove + best-module
 *     pick + strongest-connected tie-break + dirty bit + first-loop
 *     guard + single-pair pull-along.
 *   - L_after within ~1e-9 (modulo log2 ULP between V8 and libm).
 *
 * If phase 1 passes we lift to subsequent levels (which need the
 * multi-level flow alignment work tracked separately).
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
const [tracerJsonPath /*, canonCsvPath */] = args;

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = path.join(__dirname, "../../../vltanh.github.io/comdet/js");
await import(path.join(WEB, "louvain/louvain.js"));
await import(path.join(WEB, "infomap/infomap_canon.js"));

const tracer = JSON.parse(fs.readFileSync(tracerJsonPath, "utf8"));
const nLeaf = tracer.renum_to_orig.length;

// Compute the leaf-level nodeFlow_log_nodeFlow constant once. Canonical's
// MapEquation::nodeFlow_log_nodeFlow is set by initNetwork over leaf
// flows and is held constant across super-level findTopModulesRepeatedly
// iterations. Use level 0's init_flow (== leaf flows) — that's the
// first level main-Infomap captures, and active_n there equals nLeaf.
const level0 = tracer.levels.find(l => l.is_main && l.active_n === nLeaf);
let leafNodeFlowLogNodeFlow = 0;
{
  const plogp = COMDET.INFOMAP_CANON.plogp;
  for (const f of level0.init_flow) leafNodeFlowLogNodeFlow += plogp(f);
}

// Build leaf graph (compact ids 0..n-1) using the same flow values
// canonical's FlowCalculator::calcUndirectedFlow assigns: link.flow =
// 2 * w / sumWeightedDegree (w == 1, sumWeightedDegree == 2m for our
// unweighted undirected fixtures).
const m = tracer.edges.length;
const sumWeightedDegree = 2 * m;
const leafLinks = tracer.edges.map(([u, v]) => ({
  u, v, weight: 1.0, flow: 2.0 / sumWeightedDegree,
}));

// Per-level active-graph builder (same as before).
function buildActiveGraph(level) {
  const n = level.active_n;
  const lta = level.leaf_to_active;
  // Leaf-level: each undirected edge stored ONCE per input order
  // (canonical's nodeLinkMap[source][target] preserves input source/
  // target). Super-level: canonical's consolidateModules SORTS
  // (m1, m2) for undirected, so different leaf-edge orientations
  // collapse into a single (min, max) super-link. Detect leaf-level
  // by lta being the identity mapping (active_n == nLeaf and
  // lta[v] == v).
  let isLeafLevel = (n === nLeaf);
  if (isLeafLevel) {
    for (let v = 0; v < n; v++) {
      if (lta[v] !== v) { isLeafLevel = false; break; }
    }
  }
  const linkMap = new Map();
  for (const lk of leafLinks) {
    let cu = lta[lk.u];
    let cv = lta[lk.v];
    if (cu === cv) continue;
    if (!isLeafLevel && cu > cv) { const t = cu; cu = cv; cv = t; }
    const key = cu * n + cv;
    const e = linkMap.get(key);
    if (e) e.flow += lk.flow;
    else linkMap.set(key, { u: cu, v: cv, flow: lk.flow });
  }
  const links = Array.from(linkMap.values());
  // Leaf-level: preserve input encounter order (canonical's std::map
  // keyed by id sorts both source + target ASC inside source's
  // nested map, but iteration over nodeLinkMap gives source-ASC).
  // For super-level we explicitly sort.
  if (!isLeafLevel) links.sort((a, b) => a.u - b.u || a.v - b.v);
  else links.sort((a, b) => a.u - b.u || a.v - b.v);
  const outEdges = Array.from({ length: n }, () => []);
  const inEdges  = Array.from({ length: n }, () => []);
  for (const lk of links) {
    outEdges[lk.u].push(lk);
    inEdges[lk.v].push(lk);
  }
  return {
    n: n,
    links: links,
    outEdges: outEdges,
    inEdges: inEdges,
    nodeFlow:  Float64Array.from(level.init_flow),
    nodeEnter: Float64Array.from(level.init_enter),
    nodeExit:  Float64Array.from(level.init_exit),
  };
}

// Replay every main-Infomap call in trace order. Per level, the first
// call rebuilds the active-graph + Partition from the level's seed;
// subsequent calls within the same level reuse the carried partition.
const PER_VISIT_LEPS = 1e-9;
let totalVisits = 0;
let totalMismatches = 0;
let maxLDiff = 0;

let curLevel = -1;
let activeG = null;
let P = null;
let dirty = null;
let levelInitMismatch = false;
let perLevelStats = [];
let levelPass = true;
let levelVisitCount = 0;
let levelMismatchCount = 0;

function flushLevel() {
  if (curLevel >= 0) {
    perLevelStats.push({
      level: curLevel, visits: levelVisitCount, mismatches: levelMismatchCount,
      pass: levelPass,
    });
  }
}

for (let ci = 0; ci < tracer.calls.length; ci++) {
  const call = tracer.calls[ci];
  if (!tracer.levels[call.l].is_main) continue;
  if (call.l !== curLevel) {
    flushLevel();
    curLevel = call.l;
    levelPass = true;
    levelVisitCount = 0;
    levelMismatchCount = 0;
    const level = tracer.levels[call.l];
    activeG = buildActiveGraph(level);
    P = COMDET.INFOMAP_CANON.makePartition(activeG, {
      nodeFlowLogNodeFlow: leafNodeFlowLogNodeFlow,
    });
    // init_L is captured right after singleton initPartition + before
    // any predefined-modules vector is applied; verify singleton match
    // first.
    const initDelta = Math.abs(P.codelength() - level.init_L);
    if (initDelta > PER_VISIT_LEPS) {
      console.log(`level ${call.l} init_L mismatch: js=${P.codelength()}  trace=${level.init_L}  Δ=${initDelta.toExponential(3)}`);
      levelPass = false;
    }
    // If this level had a moveActiveNodesToPredefinedModules call
    // between initPartition and optimizeActiveNetwork (fineTune /
    // coarseTune do this), apply the predefined-modules vector AFTER
    // the singleton init_L check and BEFORE the level's tryMoveEach.
    if (level.predef && level.predef.length === activeG.n) {
      COMDET.INFOMAP_CANON.applyMembership(P, activeG, level.predef);
    }
    dirty = new Int8Array(activeG.n);
    for (let i = 0; i < activeG.n; i++) dirty[i] = 1;
  }
  const linkOrders = call.visits
    .filter(v => v.lo.length > 0)
    .map(v => Int32Array.from(v.lo));
  const decisionLog = [];
  try {
    COMDET.INFOMAP_CANON.tryMoveEach(P, activeG, null, {
      visitOrder: Int32Array.from(call.vo),
      linkOrders: linkOrders,
      isFirstLoop: !!call.fl,
      tuneIterationLimit: 0,
      dirty: dirty,
      onVisit: (v, moved, newM, L) => decisionLog.push({ v, moved, newM, L }),
    });
  } catch (err) {
    console.log(`call ${ci} (level ${call.l}, fl=${!!call.fl}, ${call.visits.length} visits) failed: ${err.message}`);
    console.log(`  visits-so-far in trace: ${decisionLog.length}; current trace visit:`, call.visits[decisionLog.length]);
    throw err;
  }
  const N = call.visits.length;
  if (decisionLog.length !== N) {
    console.log(`call ${ci} (level ${call.l}) visit-count mismatch: js=${decisionLog.length}  trace=${N}`);
    totalMismatches += 1;
    levelMismatchCount += 1;
    levelPass = false;
  }
  for (let k = 0; k < Math.min(decisionLog.length, N); k++) {
    const js = decisionLog[k];
    const cn = call.visits[k];
    const sameDecision = (js.moved === !!cn.m) && (js.newM === cn.n);
    const dL = Math.abs(js.L - cn.L);
    if (dL > maxLDiff) maxLDiff = dL;
    if (!sameDecision || dL > PER_VISIT_LEPS) {
      if (totalMismatches < 20) {
        console.log(`  call ${ci} (level ${call.l}) visit ${k} v=${cn.v}: js={moved=${js.moved}, newM=${js.newM}, L=${js.L}}  trace={moved=${!!cn.m}, newM=${cn.n}, L=${cn.L}}  ΔL=${dL.toExponential(3)}`);
      }
      totalMismatches += 1;
      levelMismatchCount += 1;
      levelPass = false;
    }
    levelVisitCount += 1;
    totalVisits += 1;
  }
}
flushLevel();

console.log("");
console.log(`per-level summary:`);
for (const s of perLevelStats) {
  console.log(`  level ${String(s.level).padStart(2)}  visits=${String(s.visits).padStart(5)}  mismatches=${String(s.mismatches).padStart(5)}  ${s.pass ? "PASS" : "FAIL"}`);
}
console.log(`total visits replayed: ${totalVisits}`);
console.log(`total mismatches:      ${totalMismatches}`);
console.log(`per-visit max |ΔL|     ${maxLDiff.toExponential(3)}`);

if (totalMismatches) {
  console.log("OVERALL: FAIL");
  process.exit(1);
}
console.log("OVERALL: PASS (deterministic core byte-equal across every main-Infomap call)");
process.exit(0);
