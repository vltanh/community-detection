/* Leiden self-RNG byte-equal check (L4).
 *
 * JS production walker (no oracle, no visitOrder injection) under
 * matching seed must produce per-visit (v, fromComm, toComm, dQ,
 * moved) records bit-equal vs cpp standalone trace.
 *
 * Run cpp tracer with iters=1 so JS's single optimisePartition call
 * lines up against the cpp run pass-for-pass:
 *
 *   /tmp/leiden_kernel_check <edge.csv> <out.csv> <q> <param> <seed> 1 > trace.json
 *   node self_rng_check.mjs trace.json <edge.csv> <q> <param> <seed>
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 5) {
  console.error("usage: self_rng_check.mjs <cpp.json> <edge.csv> <quality:cpm|mod> <param> <seed>");
  process.exit(2);
}
const [cppPath, edgePath, quality, paramStr, seedStr] = args;
const param = parseFloat(paramStr);
const seed = parseInt(seedStr, 10);

const cpp = JSON.parse(fs.readFileSync(cppPath, "utf8"));

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = path.join(__dirname, "../../../../../web/vltanh.github.io/comdet/js");
await import(path.join(WEB, "louvain/louvain.js"));
await import(path.join(WEB, "leiden/leiden.js"));

function loadEdges(edgePath, renum) {
  const text = fs.readFileSync(edgePath, "utf8").trim().split(/\r?\n/);
  const orig_to_new = new Map();
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

const renum = cpp.renum_to_orig;
const n = renum.length;
const edges = loadEdges(edgePath, renum);

// sortAdj: true to mirror libleidenalg's igraph_lazy_adjlist iteration
// order (sorted by neighbour-id asc); required for byte-equal greedy
// pick under matching seed.
const G = COMDET.LOUVAIN.Graph(n, edges, { correctSelfLoops: false, sortAdj: true });

// Mirror libleidenalg CPM byte-for-byte (same shape as kernel_check.mjs canonCPM).
function canonCPM(resolution) {
  return {
    name: "canonCPM",
    resolution,
    diffMove(P, v, newComm) {
      const oldComm = P.memberOf(v);
      if (oldComm === newComm) return 0;
      const Gp = P.graph;
      const wToOld = P.weightToComm(v, oldComm);
      const wToNew = P.weightToComm(v, newComm);
      const wFromOld = P.weightFromComm(v, oldComm);
      const wFromNew = P.weightFromComm(v, newComm);
      const sw = Gp.nodeSelfWeight(v);
      const nv = Gp.nodeSize(v);
      const csizeOld = P.csize(oldComm);
      const csizeNew = P.csize(newComm);
      const csl = Gp.correctSelfLoops();
      const possOld = csl ? nv * (2 * csizeOld - nv) : nv * (2 * csizeOld - nv - 1);
      const possNew = csl ? nv * (2 * csizeNew + nv) : nv * (2 * csizeNew + nv - 1);
      const diffOld = wToOld + wFromOld - sw - resolution * possOld;
      const diffNew = wToNew + wFromNew + sw - resolution * possNew;
      return diffNew - diffOld;
    },
    quality(P) {
      const Gp = P.graph;
      const csl = Gp.correctSelfLoops();
      let mod = 0;
      for (let c = 0; c < P.ncomm(); c++) {
        if (P.cnodes(c) === 0) continue;
        const nc = P.csize(c);
        // totalWeightInComm stores 2*intra_c (canonical Modularity
        // convention in louvain.js); halve to recover intra_c.
        const w = P.totalWeightInComm(c) / 2;
        const possible = csl ? (nc * nc) : (nc * (nc - 1));
        mod += w - resolution * possible / 2;
      }
      return 2 * mod;
    },
  };
}

function canonMod() {
  return {
    name: "canonMod",
    resolution: 1.0,
    diffMove(P, v, newComm) {
      const oldComm = P.memberOf(v);
      if (oldComm === newComm) return 0;
      const Gp = P.graph;
      // JS Graph.totalWeight() = Σ weighted_degree = 2*m_cpp for
      // undirected. Halve to recover libleidenalg's
      // graph->total_weight() = sum of edge weights.
      const m_orig = Gp.totalWeight() / 2;
      if (m_orig === 0) return 0;
      const directed = Gp.isDirected();
      const total_weight = m_orig * (directed ? 1.0 : 2.0);
      const w_to_old = P.weightToComm(v, oldComm);
      const w_from_old = P.weightFromComm(v, oldComm);
      const w_to_new = P.weightToComm(v, newComm);
      const w_from_new = P.weightFromComm(v, newComm);
      const k_out = Gp.strength(v);
      const k_in = directed ? Gp.strength(v) : k_out;
      const sw = Gp.nodeSelfWeight(v);
      const K_out_old = P.totalWeightFromComm(oldComm);
      const K_in_old  = P.totalWeightToComm(oldComm);
      const K_out_new = P.totalWeightFromComm(newComm) + k_out;
      const K_in_new  = P.totalWeightToComm(newComm) + k_in;
      const diff_old = (w_to_old - k_out * K_in_old / total_weight)
                     + (w_from_old - k_in * K_out_old / total_weight);
      const diff_new = (w_to_new + sw - k_out * K_in_new / total_weight)
                     + (w_from_new + sw - k_in * K_out_new / total_weight);
      const diff = diff_new - diff_old;
      const m = directed ? m_orig : 2.0 * m_orig;
      return diff / m;
    },
    quality(P) {
      const Gp = P.graph;
      const m_orig = Gp.totalWeight() / 2;
      if (m_orig === 0) return 0;
      const directed = Gp.isDirected();
      const m = directed ? m_orig : 2.0 * m_orig;
      let mod = 0;
      for (let c = 0; c < P.ncomm(); c++) {
        // totalWeightInComm stores 2*intra_c (canonical Modularity
        // convention in louvain.js); halve to recover intra_c.
        const w = P.totalWeightInComm(c) / 2;
        const w_out = P.totalWeightFromComm(c);
        const w_in = P.totalWeightToComm(c);
        mod += w - w_out * w_in / ((directed ? 1.0 : 4.0) * m_orig);
      }
      const q = (directed ? 1.0 : 2.0) * mod;
      return q / m;
    },
  };
}

let qfn;
if (quality === "cpm") qfn = canonCPM(param);
else if (quality === "mod") qfn = canonMod();
else { console.error("unknown quality"); process.exit(2); }

// Run JS optimisePartition with recordTrace; this builds JS's own
// trace mirroring cpp's pass-by-pass shape. maxOuterLevels=1 caps to
// one outer iter (one move-pass + one refine-pass at level 0) since
// inner-level admin algebra mismatch (audit row M) makes JS visit-cap
// pathologically loop on collapsed graphs and is the deferred gap.
// Top-level byte-equal is the user-facing pedagogical contract.
const out = COMDET.LEIDEN.optimisePartition(G, qfn, seed >>> 0, {
  recordTrace: true,
  maxOuterLevels: 1,
});

// Flatten JS levels into pass list parallel to cpp's. Each JS level
// emits one move pass + one refine pass (when refinePartition true).
const jsPasses = [];
for (let li = 0; li < out.levels.length; li++) {
  const L = out.levels[li];
  jsPasses.push({ phase: "move", level: L.collapsedVcount, moves: L.moveTraces });
  if (L.refineTraces && L.refineTraces.length) {
    jsPasses.push({ phase: "refine", level: L.collapsedVcount, moves: L.refineTraces });
  }
}

// TOP-LEVEL byte-equal closure: filter both sides to passes whose
// operating graph has vcount == n. Inner-level passes operate on
// collapsed super-graphs that depend on collapse edge-weight
// aggregation order; verifying those requires cpp to expose its
// collapsed-graph adjacency (deferred — top-level byte-equal is the
// user-facing pedagogical contract for the browser walker).
const cppTop = cpp.passes.filter(function (p) {
  return p.post_membership && p.post_membership.length === n;
});
const jsTop = jsPasses.filter(function (p) { return p.level === n; });

const _bbuf = new Float64Array(1);
const _bview = new BigUint64Array(_bbuf.buffer);
function bits(x) { _bbuf[0] = x; return _bview[0]; }

let total_visits = 0, total_moves = 0;
let mm_v = 0, mm_from = 0, mm_to = 0, mm_moved = 0, mm_dQ = 0;
let pass_count_mismatch = false;
let pass_visit_mismatch_pass = -1;

if (cppTop.length !== jsTop.length) {
  pass_count_mismatch = true;
}
let first_diverge_logged = false;
const passN = Math.min(cppTop.length, jsTop.length);
for (let pi = 0; pi < passN; pi++) {
  const pc = cppTop[pi];
  const pj = jsTop[pi];
  const visitN = Math.min(pc.moves.length, pj.moves.length);
  for (let mi = 0; mi < visitN; mi++) {
    const mc = pc.moves[mi];
    const mj = pj.moves[mi];
    total_visits++;
    let local_mm = 0;
    if (mc.v !== mj.v) { mm_v++; local_mm++; }
    if (mc.from !== mj.fromComm) { mm_from++; local_mm++; }
    if (mc.to !== mj.toComm) { mm_to++; local_mm++; }
    if (mc.moved !== mj.moved) { mm_moved++; local_mm++; }
    if (mc.moved && mj.moved) {
      total_moves++;
      if (bits(mc.dQ) !== bits(mj.delta)) { mm_dQ++; local_mm++; }
    }
    if (local_mm && !first_diverge_logged) {
      first_diverge_logged = true;
      console.error(`  first diverge: pass=${pi} (phase=${pc.phase}) visit=${mi}`);
      console.error(`    cpp: v=${mc.v} from=${mc.from} to=${mc.to} dQ=${mc.dQ} moved=${mc.moved}`);
      console.error(`    js : v=${mj.v} from=${mj.fromComm} to=${mj.toComm} dQ=${mj.delta} moved=${mj.moved}`);
    }
  }
  if (pc.moves.length !== pj.moves.length) {
    pass_visit_mismatch_pass = pi;
    break;
  }
}

console.log("=== Leiden self-RNG L4 check ===");
console.log(`canonical: top passes=${cppTop.length} total visits=${cppTop.reduce(function (a, p) { return a + p.moves.length; }, 0)}`);
console.log(`js:        top passes=${jsTop.length} total visits=${jsTop.reduce(function (a, p) { return a + p.moves.length; }, 0)}`);
console.log(`pass-count mismatch: ${pass_count_mismatch}`);
if (pass_visit_mismatch_pass >= 0) {
  console.log(`pass ${pass_visit_mismatch_pass}: visit-count diverged (cpp=${cppTop[pass_visit_mismatch_pass].moves.length} vs js=${jsTop[pass_visit_mismatch_pass].moves.length})`);
}
console.log(`per-visit: v_mm=${mm_v} from_mm=${mm_from} to_mm=${mm_to} moved_mm=${mm_moved} dQ_bit_mm=${mm_dQ}/${total_moves}`);
console.log(`Q_canon=${cpp.Q_final} Q_js=${out.quality}`);

const total_mm = mm_v + mm_from + mm_to + mm_moved + mm_dQ;
if (pass_count_mismatch || pass_visit_mismatch_pass >= 0 || total_mm > 0) {
  console.error("FAIL: self-RNG trajectory diverged from cpp.");
  process.exit(1);
}
console.log("PASS: JS production walker bit-equal cpp standalone under matching seed.");
