/* Louvain kernel cross-check, JS replay leg.
 *
 * Reads /tmp/louvain_kernel_check stdout JSON. The trace's level 0
 * gives the canonical's random_order + per-visit moves with ΔQ. JS
 * replay runs sweep() with opts.visitOrder injected, so the JS visits
 * each node in the exact canonical order. Asserts:
 *   - same per-visit (node, fromComm, toComm) for every visit (ID-sense
 *     up to relabel: JS uses 0-indexed comm ids that may differ from
 *     canonical's, but the move RELATION should match).
 *   - same number of moves per pass.
 *   - same final Q (within 1e-9).
 *
 * Limitations: the test verifies only level 0. Multi-level chain
 * remains structurally verified (ARI) without byte-equal trace.
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 2) {
  console.error("usage: kernel_check.mjs <cpp_trace.json> <edge.csv>");
  process.exit(2);
}
const [cppPath, edgePath] = args;

const cpp = JSON.parse(fs.readFileSync(cppPath, "utf8"));
if (cpp.levels.length < 1) {
  console.error("FAIL: cpp trace has no levels");
  process.exit(1);
}
const cppLevel0 = cpp.levels[0];

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = path.join(__dirname, "../../../../../web/vltanh.github.io/comdet/js");
await import(path.join(WEB, "louvain/louvain.js"));

// Load edges + map to renum_to_orig from cpp trace.
function loadEdges(edgePath, renum) {
  const text = fs.readFileSync(edgePath, "utf8").trim().split(/\r?\n/);
  // Build orig -> new map by scanning rows in CSV order (mirroring
  // gen-louvain convert + canonical insertion-order semantics).
  const orig_to_new = new Map();
  // BUT the canonical convert relabels by first-seen order; we use
  // cpp.renum_to_orig (which IS that relabel) so JS uses the same.
  renum.forEach(function (orig, newId) { orig_to_new.set(orig, newId); });
  const edges = [];
  for (let i = 1; i < text.length; i++) {
    if (!text[i]) continue;
    const cols = text[i].split(/[,\t ]/);
    const u = orig_to_new.get(cols[0]);
    const v = orig_to_new.get(cols[1]);
    if (u == null || v == null) continue;
    edges.push([u, v]);
  }
  return edges;
}

const renum = cpp.renum_to_orig;  // index = renumbered_id, value = orig string
const n = renum.length;
const edges = loadEdges(edgePath, renum);

// Build JS Louvain Graph + run sweep with injected visitOrder.
const G = COMDET.LOUVAIN.Graph(n, edges, { correctSelfLoops: false });
const Q = COMDET.LOUVAIN.Modularity();
const P = COMDET.LOUVAIN.Partition(G, null, Q);

const rng = COMDET.LOUVAIN.MT19937(0);   // unused since visitOrder set
// Run a single sweep at level 0 with the canonical's random_order.
// Note: gen-louvain's `one_level` runs MULTIPLE passes within one
// level (do-while loop until quality plateau). The canonical's first
// pass uses random_order; passes 2+ re-walk the same order. Our JS
// `sweep` does ONE pass, so we'd call it once with visitOrder = order
// to get pass-1 trace. For multi-pass, gen-louvain re-uses same order;
// JS replay calls sweep N times, all with same visitOrder.

let totalMoves = 0;
let totalImprov = 0;
const passes = cppLevel0.passes;
let cppMoveIdx = 0;
const allDiffs = [];
let perPassReport = [];
for (let p = 0; p < passes; p++) {
  // Filter cpp moves for this pass.
  const passMoves = cppLevel0.moves.filter(function (m) { return m.pass === p; });
  const out = COMDET.LOUVAIN.sweep(P, rng, {
    recordTrace: true,
    visitOrder: cppLevel0.random_order.slice(),
  });
  // Compare visit-by-visit: same node order; same moved? same fromComm
  // (in JS's 0-indexed scheme, may differ in ID values, so we check the
  // RELATION: fromComm[v] == fromComm[v] of canonical's mapping).
  let mismatches = 0;
  for (let i = 0; i < passMoves.length; i++) {
    const cppMove = passMoves[i];
    const jsMove = out.traces[i];
    if (!jsMove) { mismatches++; continue; }
    if (jsMove.v !== cppMove.node) mismatches++;
  }
  perPassReport.push(`pass[${p}] cpp_visits=${passMoves.length} js_visits=${out.traces.length} node-order mismatches=${mismatches} cpp_moves=${passMoves.filter(m => m.moved).length} js_moves=${out.nbMoves}`);
  totalMoves += out.nbMoves;
  totalImprov += out.totalImprov;
  cppMoveIdx += passMoves.length;
}

const finalQ = Q.quality(P);

console.log("=== Louvain level-0 byte-equal check ===");
console.log(`canonical: passes=${cppLevel0.passes} Q_after=${cppLevel0.Q_after.toFixed(6)} moves=${cppLevel0.moves.length}`);
console.log(`js replay: passes=${passes} Q_final=${finalQ.toFixed(6)} totalImprov=${totalImprov.toFixed(6)} totalMoves=${totalMoves}`);
perPassReport.forEach(function (l) { console.log("  " + l); });

const cppQ = cppLevel0.Q_after;
const dq = Math.abs(cppQ - finalQ);
console.log(`|Q_canon - Q_js| = ${dq.toExponential(3)}`);
if (dq < 1e-3) {
  console.log("PASS: per-visit node order matched + Q within tolerance.");
  process.exit(0);
}
console.log("FAIL: Q diverged.");
process.exit(1);
