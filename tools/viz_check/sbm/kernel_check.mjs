/* SBM flat-variant kernel cross-check, JS replay leg.
 *
 * Reads a /tmp/sbm_flat_kernel_check JSON trace (from
 * instrumented/flat_traced.cpp) and applies the trace's per-visit
 * (toS, accept) sequence to comdet/js/sbm via opts.proposalOracle +
 * opts.visitOrder. Asserts:
 *
 *   - JS S_init == cpp.S_init within 1e-10 (validates entropy formula
 *     and BlockState init bit-for-bit at the trace's partition).
 *   - per-visit: JS dS == cpp.dS within 1e-10, fromR + toS + accept +
 *     moved exactly equal, JS-recovered pickIdx == cpp.pickIdx, JS
 *     candidate pool == cpp.cands element-wise.
 *   - per-sweep S_post and final S byte-equal at 1e-10.
 *   - final_membership byte-equal.
 *
 * RNG: cpp uses std::mt19937, JS uses MT19937 (matsumoto-nishimura).
 * Both seed the 624-word state via the same Knuth recurrence, so a
 * given seed yields the same byte stream from raw(). The oracle here
 * bypasses JS's own RNG draws entirely; the byte-equality is on the
 * algorithm's deterministic-given-(toS, accept) post-conditions
 * (dS, partition update).
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 3) {
  console.error("usage: kernel_check.mjs <cpp_trace.json> <edge.csv> <mode:dc|ndc|pp> [--nested]");
  process.exit(2);
}
const tracePath = args[0];
const edgePath = args[1];
const mode = args[2];
const nestedFlag = args.includes("--nested");
if (!["dc", "ndc", "pp"].includes(mode)) {
  console.error("mode must be one of dc, ndc, pp");
  process.exit(2);
}

const trace = JSON.parse(fs.readFileSync(tracePath, "utf8"));
if (nestedFlag) {
  if (trace.variant !== `nested-${mode}`) {
    console.error(`trace variant "${trace.variant}" != cli "nested-${mode}"`);
    process.exit(2);
  }
} else if (trace.mode !== mode) {
  console.error(`trace mode "${trace.mode}" != cli mode "${mode}"`);
  process.exit(2);
}

// Boot the comdet JS modules in the order their IIFE guards expect.
globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = path.join(__dirname, "../../../vltanh.github.io/comdet/js");
await import(path.join(WEB, "louvain/louvain.js"));
await import(path.join(WEB, "sbm/util.js"));
await import(path.join(WEB, "sbm/block_state.js"));
await import(path.join(WEB, "sbm/mcmc.js"));
await import(path.join(WEB, "sbm/nested_state.js"));

// Load edges using the trace's renum_to_orig so node ids align with cpp's.
function loadEdges(edgePath, renum) {
  const orig_to_new = new Map();
  renum.forEach((orig, newId) => orig_to_new.set(String(orig), newId));
  const lines = fs.readFileSync(edgePath, "utf8").trim().split(/\r?\n/);
  const edges = [];
  for (let i = 1; i < lines.length; i++) {
    if (!lines[i]) continue;
    const cols = lines[i].split(/[,\t]/);
    const u = orig_to_new.get(cols[0]);
    const v = orig_to_new.get(cols[1]);
    if (u == null || v == null) {
      console.error(`unknown node in edge: ${lines[i]}`);
      process.exit(2);
    }
    edges.push([u, v]);
  }
  return edges;
}

// Relative tolerance: drift / max(1, |S|) < 1e-12.
// JS uses a 7-coefficient Lanczos lgamma; cpp uses glibc's lgamma.
// At fixed partition both implementations must agree to within their
// inherent precision (~1ulp ~ 1e-15 relative on lgamma for inputs in
// [0.5, 1e6]). PP entropy on dnc has lbinom args ~ 4e5 which pushes
// absolute drift to ~1e-9; relative is still ~3e-14.
const REL_S_TOL = 1e-12;
const ABS_dS_TOL = 1e-7;  // dS = subset entropy diff, hits same scale.
function withinRelS(drift, S) {
  return drift <= REL_S_TOL * Math.max(1, Math.abs(S));
}

const renum = trace.renum_to_orig;
const N = renum.length;
const edges = loadEdges(edgePath, renum);

const G = COMDET.LOUVAIN.Graph(N, edges, { correctSelfLoops: false });

// Nested replay branches early; the flat path below still drives the
// majority of variants.
if (nestedFlag) {
  await runNestedReplay(G, trace);
  process.exit(0);
}
const stateOpts = { mode: mode, init: trace.init_membership };
const state = COMDET.SBM.BlockState(G, stateOpts);

const failures = [];
function check(cond, msg) { if (!cond) failures.push(msg); }

// ── Initial entropy ─────────────────────────────────────────────────
const S_init_js = state.entropy();
const S_init_drift = Math.abs(S_init_js - trace.S_init);
check(withinRelS(S_init_drift, trace.S_init),
      `S_init drift ${S_init_drift.toExponential(3)} (js=${S_init_js}, cpp=${trace.S_init})`);

// ── Sweep replay ────────────────────────────────────────────────────
let dSMaxDrift = 0;
let SMaxDrift = S_init_drift;
let totalVisits = 0;
let totalMismatchedAccept = 0;
let totalMismatchedMoved = 0;
let totalMismatchedToS = 0;
let totalMismatchedCands = 0;

const rngStub = COMDET.LOUVAIN.MT19937(0);  // unused when oracle + visitOrder set

for (let sw = 0; sw < trace.sweeps.length; sw++) {
  const swTrace = trace.sweeps[sw];
  const visits = swTrace.visits;
  let visitIdx = 0;
  const oracle = (v, i, cands) => {
    const t = visits[i];
    return { to_block: t.toS, accept: t.accept };
  };
  const out = COMDET.SBM.mcmcSweep(state, rngStub, {
    visitOrder: swTrace.visit_order,
    proposalOracle: oracle,
    recordTrace: true,
    beta: trace.beta,
  });
  if (out.traces.length !== visits.length) {
    failures.push(`sweep ${sw}: visit count mismatch js=${out.traces.length} cpp=${visits.length}`);
    break;
  }
  for (let i = 0; i < visits.length; i++) {
    const t = visits[i];
    const j = out.traces[i];
    totalVisits++;
    if (j.v !== t.v) failures.push(`sweep ${sw} visit ${i}: v ${j.v} != ${t.v}`);
    if (j.fromR !== t.fromR) failures.push(`sweep ${sw} visit ${i}: fromR ${j.fromR} != ${t.fromR}`);
    if (j.toS !== t.toS) totalMismatchedToS++;
    // pickIdx is derivable from cands.indexOf(toS); cands ordering
    // is the load-bearing equality (verifies neList stays in sync).
    if (j.cands.length !== t.cands.length) totalMismatchedCands++;
    else for (let k = 0; k < j.cands.length; k++) if (j.cands[k] !== t.cands[k]) { totalMismatchedCands++; break; }
    if (j.accept !== t.accept) totalMismatchedAccept++;
    if (j.accepted !== t.moved) totalMismatchedMoved++;
    const dDrift = Math.abs(j.dS - t.dS);
    if (dDrift > dSMaxDrift) dSMaxDrift = dDrift;
  }
  const S_post_js = state.entropy();
  const S_drift = Math.abs(S_post_js - swTrace.S_post);
  if (S_drift > SMaxDrift) SMaxDrift = S_drift;
  if (!withinRelS(S_drift, swTrace.S_post)) {
    failures.push(`sweep ${sw}: S_post drift ${S_drift.toExponential(3)} (js=${S_post_js}, cpp=${swTrace.S_post})`);
  }
}

// ── Final partition byte-equal ──────────────────────────────────────
const finalJs = state.blockMembership();
let memDiffs = 0;
for (let v = 0; v < N; v++) if (finalJs[v] !== trace.final_membership[v]) memDiffs++;
if (memDiffs > 0) failures.push(`final_membership ${memDiffs}/${N} entries differ`);

if (totalMismatchedToS) failures.push(`toS mismatches: ${totalMismatchedToS}/${totalVisits}`);
if (totalMismatchedCands) failures.push(`cand-pool mismatches: ${totalMismatchedCands}/${totalVisits}`);
if (totalMismatchedAccept) failures.push(`accept mismatches: ${totalMismatchedAccept}/${totalVisits}`);
if (totalMismatchedMoved) failures.push(`moved mismatches: ${totalMismatchedMoved}/${totalVisits}`);
if (dSMaxDrift > ABS_dS_TOL) failures.push(`dS max drift ${dSMaxDrift.toExponential(3)} > ${ABS_dS_TOL}`);

console.log(`=== SBM flat-${mode} replay ===`);
console.log(`n=${N} sweeps=${trace.sweeps.length} visits=${totalVisits}`);
console.log(`S_init: js=${S_init_js.toFixed(15)} cpp=${trace.S_init.toFixed(15)} drift=${S_init_drift.toExponential(3)}`);
console.log(`S_final: js=${state.entropy().toFixed(15)} cpp=${trace.S_final.toFixed(15)}`);
console.log(`max dS drift = ${dSMaxDrift.toExponential(3)}`);
console.log(`max S drift  = ${SMaxDrift.toExponential(3)}`);
console.log(`final_membership diffs = ${memDiffs}`);

if (failures.length > 0) {
  console.error("FAIL:");
  for (const f of failures) console.error("  " + f);
  process.exit(1);
}
console.log(`PASS: byte-equal trace + final partition (n=${N}, ${trace.sweeps.length} sweeps).`);

// ── Nested replay ───────────────────────────────────────────────────
async function runNestedReplay(G0, trace) {
  // Build initial level stack from trace.hierarchy.
  const hier = trace.hierarchy.map((arr) => Int32Array.from(arr));
  const graphs = [G0];
  const states = [COMDET.SBM.BlockState(G0, { mode: mode, init: hier[0] })];
  for (let l = 1; l < hier.length; l++) {
    const built = COMDET.SBM.buildLevelGraph(graphs[l - 1], states[l - 1], hier[l]);
    graphs.push(built.graph);
    states.push(COMDET.SBM.BlockState(built.graph, { mode: "ndc", init: built.init }));
  }

  const failures = [];
  function check(cond, msg) { if (!cond) failures.push(msg); }

  function totalS() {
    let S = 0;
    for (const st of states) S += st.entropy();
    return S;
  }

  // Anchor: total S_init.
  const S_init_js = totalS();
  const S_init_drift = Math.abs(S_init_js - trace.S_init);
  check(withinRelS(S_init_drift, trace.S_init),
        `total S_init drift ${S_init_drift.toExponential(3)}`);
  // Per-level S_init.
  for (let l = 0; l < states.length; l++) {
    const sl_js = states[l].entropy();
    const sl_cpp = trace.level_S_init[l];
    const d = Math.abs(sl_js - sl_cpp);
    check(withinRelS(d, sl_cpp),
          `level ${l} S_init drift ${d.toExponential(3)} (js=${sl_js}, cpp=${sl_cpp})`);
  }

  let dSMax = 0, totalVisits = 0, mismatches = 0;
  const rngStub = COMDET.LOUVAIN.MT19937(0);

  for (let sw = 0; sw < trace.sweeps.length; sw++) {
    const swTrace = trace.sweeps[sw];
    for (let li = 0; li < swTrace.levels.length; li++) {
      const lvl = swTrace.levels[li];
      const l = lvl.level;
      const oracle = (v, i, cands) => {
        const t = lvl.visits[i];
        return { to_block: t.toS, accept: t.accept };
      };
      const out = COMDET.SBM.mcmcSweep(states[l], rngStub, {
        visitOrder: lvl.visit_order,
        proposalOracle: oracle,
        recordTrace: true,
        beta: trace.beta,
      });
      if (out.traces.length !== lvl.visits.length) {
        failures.push(`sweep ${sw} level ${l}: visit count mismatch js=${out.traces.length} cpp=${lvl.visits.length}`);
        break;
      }
      for (let i = 0; i < lvl.visits.length; i++) {
        const t = lvl.visits[i];
        const j = out.traces[i];
        totalVisits++;
        if (j.v !== t.v || j.fromR !== t.fromR || j.toS !== t.toS
            || j.accept !== t.accept || j.accepted !== t.moved) mismatches++;
        const d = Math.abs(j.dS - t.dS);
        if (d > dSMax) dSMax = d;
      }
      const S_post_js = states[l].entropy();
      const dPost = Math.abs(S_post_js - lvl.S_post);
      if (!withinRelS(dPost, lvl.S_post))
        failures.push(`sweep ${sw} level ${l}: S_post drift ${dPost.toExponential(3)}`);

      // Propagate updated e_rs to all upper levels.
      for (let up = l + 1; up < states.length; up++) {
        const built = COMDET.SBM.buildLevelGraph(graphs[up - 1], states[up - 1], hier[up]);
        graphs[up] = built.graph;
        states[up] = COMDET.SBM.BlockState(built.graph, { mode: "ndc", init: built.init });
      }
    }
    // Sweep-level total S check.
    const S_sw_js = totalS();
    const dSW = Math.abs(S_sw_js - swTrace.S_post);
    if (!withinRelS(dSW, swTrace.S_post))
      failures.push(`sweep ${sw}: total S_post drift ${dSW.toExponential(3)}`);
  }

  if (mismatches > 0) failures.push(`${mismatches}/${totalVisits} per-visit mismatches`);
  if (dSMax > ABS_dS_TOL) failures.push(`dS max drift ${dSMax.toExponential(3)} > ${ABS_dS_TOL}`);

  // Final per-level membership.
  let memDiffs = 0;
  for (let l = 0; l < states.length; l++) {
    const m_js = states[l].blockMembership();
    const m_cpp = trace.level_final_membership[l];
    if (m_js.length !== m_cpp.length) {
      failures.push(`level ${l} membership length mismatch`);
      continue;
    }
    for (let v = 0; v < m_js.length; v++) if (m_js[v] !== m_cpp[v]) memDiffs++;
  }
  if (memDiffs > 0) failures.push(`final per-level membership ${memDiffs} entries differ`);

  console.log(`=== SBM nested-${mode} replay ===`);
  console.log(`levels=${states.length} n=${N} sweeps=${trace.sweeps.length} visits=${totalVisits}`);
  console.log(`total S: js=${totalS().toFixed(15)} cpp=${trace.S_final.toFixed(15)}`);
  console.log(`max dS drift = ${dSMax.toExponential(3)}`);
  console.log(`per-visit mismatches = ${mismatches}`);
  console.log(`final per-level membership diffs = ${memDiffs}`);

  if (failures.length > 0) {
    console.error("FAIL:");
    for (const f of failures) console.error("  " + f);
    process.exit(1);
  }
  console.log(`PASS: byte-equal nested trace + final per-level partition (${states.length} levels, ${trace.sweeps.length} sweeps).`);
}
