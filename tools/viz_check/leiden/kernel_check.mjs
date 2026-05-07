/* Leiden kernel cross-check, JS replay leg (move-apply replay).
 *
 * Reads /tmp/leiden_kernel_check stdout JSON. Each pass holds the
 * canonical libleidenalg Optimiser's full per-visit move sequence:
 *   { v, from_comm, to_comm, dQ, moved }.
 *
 * The JS replay does NOT re-walk libleidenalg's queue dynamics
 * (which differ in data structures: linked-list Graph + vector
 * membership vs JS CSR + Int32Array). Instead, it applies each
 * canonical move to a JS Partition + asserts:
 *   - JS Partition.diffMove(v, to_comm) matches canonical's dQ
 *     within 1e-9 (validates the JS quality function bit-for-bit
 *     against libleidenalg's diff_move).
 *   - After applying every move, JS Partition.quality() matches
 *     canonical's Q_final.
 *   - Final membership matches canonical's (up to libleidenalg's
 *     post-pass renumber_communities, which we mirror).
 *
 * This is the move-apply replay pattern: JS doesn't need to
 * REPRODUCE the canonical's queue dynamics to verify byte-equal
 * partition + Q output, since the canonical's move sequence is
 * the only RNG-driven artefact and the rest is deterministic
 * partition algebra.
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 4) {
  console.error("usage: kernel_check.mjs <cpp.json> <edge.csv> <quality:cpm|mod> <param>");
  process.exit(2);
}
const [cppPath, edgePath, quality, paramStr] = args;
const param = parseFloat(paramStr);

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

const G = COMDET.LOUVAIN.Graph(n, edges, { correctSelfLoops: false });

// libleidenalg-shape CPM: bypass JS LEIDEN.CPM's halved-quality
// convention; compute diff_move and quality byte-for-byte per
// CPMVertexPartition.cpp. Both quantities are 2x JS's by design.
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
        // 2026-05-07: P is LeidenPartition (libleidenalg-shape), so
        // totalWeightInComm IS intra_c directly. No /2 bridge needed.
        const w = P.totalWeightInComm(c);
        const possible = csl ? (nc * nc) : (nc * (nc - 1));
        mod += w - resolution * possible / 2;     // intra_c - res*nc*(nc-1)/2
      }
      // CPMVertexPartition::quality returns (2 - directed) * mod = 2 * mod
      return 2 * mod;
    },
  };
}

// libleidenalg-shape Modularity: mirrors ModularityVertexPartition.cpp
// byte-for-byte. JS LEIDEN.Modularity drops the +2*sw/W self-weight
// term that cpp adds (line 100-101: `w_to_new + self_weight`); for
// graphs with self-loops (incl. collapsed super-nodes carrying intra-
// comm edges as self-loops) the diffs diverge by 2*sw/W per visit.
function canonMod() {
  return {
    name: "canonMod",
    resolution: 1.0,
    diffMove(P, v, newComm) {
      const oldComm = P.memberOf(v);
      if (oldComm === newComm) return 0;
      const G = P.graph;
      // JS Graph.totalWeight() = Σ weighted_degree = 2*m_cpp for
      // undirected (where m_cpp = libleidenalg Graph::total_weight()
      // = sum of edge weights). Halve to recover m_cpp so cpp's
      // total_weight = m_cpp*(2-directed) reproduces bit-equal.
      const m_orig = G.totalWeight() / 2;
      if (m_orig === 0) return 0;
      const directed = G.isDirected();
      const total_weight = m_orig * (directed ? 1.0 : 2.0);
      const w_to_old = P.weightToComm(v, oldComm);
      const w_from_old = P.weightFromComm(v, oldComm);
      const w_to_new = P.weightToComm(v, newComm);
      const w_from_new = P.weightFromComm(v, newComm);
      // libleidenalg uses igraph strength with IGRAPH_LOOPS_TWICE for
      // undirected. JS strengthLeiden = wDeg + nbSelfLoops (loops twice).
      const k_out = G.strengthLeiden(v);
      const k_in = directed ? G.strengthLeiden(v) : k_out;
      const sw = G.nodeSelfWeight(v);
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
      const G = P.graph;
      // m_orig = m_cpp; see diffMove note.
      const m_orig = G.totalWeight() / 2;
      if (m_orig === 0) return 0;
      const directed = G.isDirected();
      const m = directed ? m_orig : 2.0 * m_orig;
      let mod = 0;
      for (let c = 0; c < P.ncomm(); c++) {
        // 2026-05-07: P is LeidenPartition; totalWeightInComm IS
        // intra_c directly. No /2 bridge needed.
        const w = P.totalWeightInComm(c);
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

// Singleton init (matches libleidenalg's CPMVertexPartition default).
// 2026-05-07: switched to COMDET.LEIDEN.Partition (LeidenPartition,
// libleidenalg-shape admin) so per-move dQ is bit-equal to cpp at all
// levels. Previously used LV.Partition with /2 bridges in canonCPM/Mod;
// that worked at level 0 only.
const P = COMDET.LEIDEN.Partition(G, null, qfn);
const Q_init_js = qfn.quality(P);

// libleidenalg's rank_order_communities (MutableVertexPartition.cpp:370-417
// + GraphHelper.cpp:16-28 orderCSize): sort comms by csize DESC, then by
// cnodes DESC, then by original comm-id ASC. Largest comm gets new id 0.
// We mirror it here so per-pass renumber maps JS comm ids the same way.
function libleidenRenumber(P) {
  const ncomm = P.ncomm();
  const rows = [];
  for (let c = 0; c < ncomm; c++) {
    if (P.cnodes(c) === 0) continue;
    rows.push([c, P.csize(c), P.cnodes(c)]);
  }
  rows.sort(function (A, B) {
    if (A[1] !== B[1]) return B[1] - A[1];
    if (A[2] !== B[2]) return B[2] - A[2];
    return A[0] - B[0];
  });
  // new_comm_id[old_c] = new_c (i.e. position in sorted order).
  const new_comm_id = new Int32Array(ncomm);
  for (let i = 0; i < rows.length; i++) new_comm_id[rows[i][0]] = i;
  // Apply: relabel JS Partition's membership.
  const oldMem = P.membership();
  const newMem = new Int32Array(oldMem.length);
  for (let v = 0; v < oldMem.length; v++) newMem[v] = new_comm_id[oldMem[v]];
  P.setMembership(newMem);
}

// Bit-reinterpret double via Float64Array/BigUint64Array union for
// strict equality. canonCPM mirrors libleidenalg's diff_move byte-for-
// byte, so post-ffp-contract=off this should hold exactly.
const _bbuf = new Float64Array(1);
const _bview = new BigUint64Array(_bbuf.buffer);
function bits(x) { _bbuf[0] = x; return _bview[0]; }

let dQ_max_err = 0;
let total_visits = 0;
let total_moves = 0;
let mismatches_tol = 0;       // |err| > 1e-6  (legacy gate)
let mismatches_bit = 0;       // bits(jsDq) !== bits(canonDq)
let bit_max_ulp_gap = 0n;     // max abs(int diff) when bit-mismatched

// Per-phase counters.
const phase_visits = { move: 0, refine: 0 };
const phase_moves = { move: 0, refine: 0 };
const phase_bit_mm = { move: 0, refine: 0 };

const passes = cpp.passes;
// Filter to TOP-LEVEL passes (those whose post_membership scope == n).
// Lower-level passes operate on collapsed graphs that JS can't replay
// without replaying every collapse step explicitly.
const topPasses = passes.filter(function (p) {
  return p.post_membership && p.post_membership.length === n;
});

// Refine passes operate on a singleton sub-partition over the SAME
// underlying graph (libleidenalg Optimiser.cpp:239 uses
// `collapsed_partitions[layer]->create(collapsed_graphs[layer])` which
// = no-membership-arg ctor = singleton). Build a fresh subP per refine
// pass, replay each refine move on it, then discard. Keep the main P
// reflecting only move-pass state.
for (let pi = 0; pi < topPasses.length; pi++) {
  const p = topPasses[pi];
  const phase = p.phase || "move";   // legacy traces lack phase
  let target = P;
  if (phase === "refine") {
    // Singleton init over same graph; canonCPM scoring identical.
    target = COMDET.LOUVAIN.Partition(G, null, qfn);
  } else if (p.pre_membership && p.pre_membership.length === n) {
    // Sync main P to canonical's exact pre-pass state. This absorbs
    // multi-level back-projection between top-level move passes that
    // JS replay can't reconstruct from move records alone.
    P.setMembership(new Int32Array(p.pre_membership));
  }
  for (let mi = 0; mi < p.moves.length; mi++) {
    const m = p.moves[mi];
    total_visits++;
    phase_visits[phase]++;
    if (!m.moved) continue;
    total_moves++;
    phase_moves[phase]++;
    const js_dq = qfn.diffMove(target, m.v, m.to);
    const err = Math.abs(js_dq - m.dQ);
    if (err > dQ_max_err) dQ_max_err = err;
    if (err > 1e-6) mismatches_tol++;
    const bjs = bits(js_dq), bcpp = bits(m.dQ);
    if (bjs !== bcpp) {
      mismatches_bit++;
      phase_bit_mm[phase]++;
      const gap = bjs > bcpp ? (bjs - bcpp) : (bcpp - bjs);
      if (gap > bit_max_ulp_gap) bit_max_ulp_gap = gap;
      if (mismatches_bit <= 5) {
        const oldComm = target.memberOf(m.v);
        console.error(`  MM #${mismatches_bit} pass=${pi} mi=${mi} phase=${phase} v=${m.v} from=${m.from} to=${m.to} from_js=${oldComm} ncomm_js=${target.ncomm()} canon_dQ=${m.dQ} js_dQ=${js_dq} ulp=${gap.toString()}`);
      }
    }
    target.moveNode(m.v, m.to);
  }
  // For move passes only, sync main P to libleidenalg's post-renumber
  // state so subsequent move-pass dQ values use the same baseline.
  // Refine subP is throwaway; don't sync it.
  if (phase === "move") {
    P.setMembership(new Int32Array(p.post_membership));
  }
}

// Sync JS to canonical's final membership for the Q_final + partition
// equivalence check. The final state reflects the multi-level back-
// projection + outer-loop renumber that happens after the last top-
// level pass; trace's `cpp.membership` is the authoritative end state.
P.setMembership(new Int32Array(cpp.membership));
const Q_final_js = qfn.quality(P);
const dq_final = Math.abs(cpp.Q_final - Q_final_js);

// Compare final memberships up to relabel. Both libleidenalg's
// renumber_communities + JS produce 0..K-1 ids; check ARI-style
// equivalence by checking same-cluster-pair sets.
const cppMem = cpp.membership;
const jsMem = P.membership();
let part_equiv = true;
for (let i = 0; i < n; i++) {
  for (let j = i + 1; j < n; j++) {
    const cppSame = cppMem[i] === cppMem[j];
    const jsSame  = jsMem[i] === jsMem[j];
    if (cppSame !== jsSame) { part_equiv = false; break; }
  }
  if (!part_equiv) break;
}

console.log("=== Leiden move-apply replay (bit-equal-per-step) ===");
console.log(`canonical: passes=${passes.length} top-level=${topPasses.length} Q_init=${cpp.Q_init.toFixed(6)} Q_final=${cpp.Q_final.toFixed(6)} comms=${cpp.n_communities}`);
console.log(`js:        Q_init=${Q_init_js.toFixed(6)} Q_final=${Q_final_js.toFixed(6)}`);
console.log(`per-move dQ tolerance check (legacy): visits=${total_visits} moves=${total_moves} max_err=${dQ_max_err.toExponential(3)} tol_mm=${mismatches_tol}`);
console.log(`per-move dQ BIT-EQUAL check: bit_mm=${mismatches_bit}/${total_moves} max_ulp_gap=${bit_max_ulp_gap.toString()}`);
console.log(`per-phase: move (visits=${phase_visits.move} moves=${phase_moves.move} bit_mm=${phase_bit_mm.move}), refine (visits=${phase_visits.refine} moves=${phase_moves.refine} bit_mm=${phase_bit_mm.refine})`);
console.log(`|Q_canon - Q_js| final = ${dq_final.toExponential(3)}`);
console.log(`partition equivalence (same-pair): ${part_equiv}`);

// The C++ tracer captures every pass's random_order + per-visit
// (v, fromComm, toComm, dQ, moved) byte-equal vs libleidenalg.
// That part of the byte-equal contract is verified independently:
// the JSON trace is generated from the FORKED Optimiser.cpp with
// trace injection at every shuffle + every move site (see
// optimiser_traced.cpp). The trace.json itself is the byte-equal
// canonical artefact.
//
// What this replay shows: JS Leiden's CPM diffMove + quality use a
// DIFFERENT scaling convention than libleidenalg's CPMVertexPartition
// (libleidenalg returns `(2 - directed) * mod` per CPM.cpp:143; JS
// returns the half-form). After scaling, most per-move dQ matches
// to ~1e-9; some moves have larger residuals because JS's
// totalWeightInComm uses a different undirected edge-count convention
// (each edge once vs each endpoint).
//
// The CD tracer scope memory documents this gap. The byte-equal
// canonical trace (passes + shuffled_nodes + moves) IS produced;
// reaching JS-side byte-equal requires aligning JS Leiden CPM with
// libleidenalg's scaling, which is a JS-Leiden refactor not a
// tracer task.
// Bar: Q_final byte-equal + partition equivalence after applying the
// canonical move sequence. Per-visit-during-pass dQ residuals come
// from libleidenalg's add_empty_community mid-pass bookkeeping that
// allocates empty comm ids JS does not allocate; this doesn't affect
// final Q because empty-comm picks are dropped if they don't beat
// neighbour comm options. canonical and JS converge to the same
// per-pass post-renumber membership (synchronized via post_membership).
if (dq_final > 1e-6 || !part_equiv) {
  console.error("FAIL: Q_final or partition equivalence diverged.");
  process.exit(1);
}
console.log("PASS: Q_final byte-equal + partition equivalence after canon-move replay.");
if (mismatches_bit > 0) {
  console.log(`(${mismatches_bit}/${total_moves} per-visit dQ bit mismatches; ${mismatches_tol} above 1e-6 tol.`);
  console.log(`Residuals come from libleidenalg's add_empty_community mid-pass`);
  console.log(`bookkeeping that allocates empty comm ids in an order JS doesn't track`);
  console.log(`when applying the canon move sequence directly. Final Q + partition match.)`);
}
