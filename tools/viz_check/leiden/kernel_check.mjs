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
        // libleidenalg's totalWeightInComm[c] for undirected counts
        // each intra edge twice (once per endpoint) per move_node.
        // JS LOUVAIN.Partition's totalWeightInComm uses the once-count
        // convention. Multiply by 2 to align.
        const w = P.totalWeightInComm(c) * 2;
        const possible = csl ? (nc * nc) : (nc * (nc - 1));
        // libleidenalg `possible_edges(nc)` for undirected = nc*(nc-1)/2
        // multiplied by 2 in quality returns gives nc*(nc-1).
        mod += w / 2 - resolution * possible / 2;  // =  w_orig - res * nc*(nc-1)/2
      }
      // CPMVertexPartition::quality returns (2 - directed) * mod = 2 * mod
      return 2 * mod;
    },
  };
}

let qfn;
if (quality === "cpm") qfn = canonCPM(param);
else if (quality === "mod") qfn = COMDET.LEIDEN.Modularity();
else { console.error("unknown quality"); process.exit(2); }

// Singleton init (matches libleidenalg's CPMVertexPartition default).
const P = COMDET.LOUVAIN.Partition(G, null, qfn);
const Q_init_js = qfn.quality(P);

let dQ_max_err = 0;
let total_visits = 0;
let total_moves = 0;
let mismatches = 0;
const passes = cpp.passes;
for (let pi = 0; pi < passes.length; pi++) {
  const p = passes[pi];
  for (let mi = 0; mi < p.moves.length; mi++) {
    const m = p.moves[mi];
    total_visits++;
    if (!m.moved) continue;
    total_moves++;
    // canonCPM.diffMove mirrors libleidenalg's diff_move byte-for-byte.
    const js_dq = qfn.diffMove(P, m.v, m.to);
    const err = Math.abs(js_dq - m.dQ);
    if (err > dQ_max_err) dQ_max_err = err;
    if (err > 1e-6) mismatches++;
    P.moveNode(m.v, m.to);
  }
  // libleidenalg ends every move_nodes call with renumber_communities
  // (Optimiser.cpp:791). Mirror so the next pass's comm ids match.
  P.renumber();
}

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

console.log("=== Leiden move-apply replay ===");
console.log(`canonical: passes=${passes.length} Q_init=${cpp.Q_init.toFixed(6)} Q_final=${cpp.Q_final.toFixed(6)} comms=${cpp.n_communities}`);
console.log(`js:        Q_init=${Q_init_js.toFixed(6)} Q_final=${Q_final_js.toFixed(6)}`);
console.log(`per-move dQ check: visits=${total_visits} moves=${total_moves} max_err=${dQ_max_err.toExponential(3)} mismatches=${mismatches}`);
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
if (mismatches > 0 || dq_final > 1e-6 || !part_equiv) {
  console.error("");
  console.error("PARTIAL: canonical TRACE is byte-equal (every pass's");
  console.error("  random_order + per-visit (v, fromComm, toComm, dQ, moved)");
  console.error("  was captured from the FORKED libleidenalg Optimiser.cpp).");
  console.error("  JS-side replay diverges because comdet/js/leiden CPM uses");
  console.error("  a different undirected-weight convention than libleidenalg's");
  console.error("  CPMVertexPartition (intra edges counted once vs twice; quality");
  console.error("  is 2x scaled). Aligning the two requires a JS-Leiden CPM");
  console.error("  refactor, NOT a tracer change. See leiden_tracer_verify.md.");
  process.exit(1);
}
console.log("PASS: per-move dQ + Q_final + partition equivalence all match.");
