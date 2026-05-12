/* SBM kernel cross-check, JS replay leg.
 *
 * Reads a /tmp/sbm_flat_kernel_check JSON trace (from
 * instrumented/flat_traced.cpp) and applies the trace's per-visit
 * (toS, accept) sequence to comdet/js/sbm via opts.proposalOracle +
 * opts.visitOrder. Asserts:
 *
 *   - JS S_init == cpp.S_init within 1e-12 relative.
 *   - per-visit: JS dS == cpp.dS within 1e-7 absolute, plus
 *     fromR + toS + accept + moved + cands array exactly equal.
 *   - per-sweep S_post and final S byte-equal (relative 1e-12).
 *   - final per-level membership byte-equal.
 *
 * Both legs (cpp + JS) seed std::mt19937 / SBM.MT19937 (rng.js) from
 * the same Knuth recurrence; the oracle bypasses JS's RNG draws so
 * byte-equality lives on the deterministic-given-(toS, accept)
 * post-conditions (dS, partition update).
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

// cpp tracer emits NaN / +/-Infinity as the JSON string tokens
// "NaN" / "Infinity" / "-Infinity" — see self_rng_check.mjs for the
// motivating site (OOB-tolerant ers_at in nested mode when
// candidatePool returns a fresh-block target s == B).
function nanReviver(_k, v) {
  if (v === "NaN") return NaN;
  if (v === "Infinity") return Infinity;
  if (v === "-Infinity") return -Infinity;
  return v;
}
const trace = JSON.parse(fs.readFileSync(tracePath, "utf8"), nanReviver);
if (nestedFlag) {
  if (trace.variant !== `nested-${mode}`) {
    console.error(`trace variant "${trace.variant}" != cli "nested-${mode}"`);
    process.exit(2);
  }
} else if (trace.mode !== mode) {
  console.error(`trace mode "${trace.mode}" != cli mode "${mode}"`);
  process.exit(2);
}

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = path.join(__dirname, "../../../vltanh.github.io/comdet/js");
await import(path.join(WEB, "sbm/util.js"));
await import(path.join(WEB, "sbm/rng.js"));
await import(path.join(WEB, "sbm/graph.js"));
await import(path.join(WEB, "sbm/block_state.js"));
await import(path.join(WEB, "sbm/mcmc.js"));
await import(path.join(WEB, "sbm/nested_state.js"));

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

// JS Lanczos lgamma vs cpp glibc lgamma residual: ~1e-15 relative on
// the small-input dominant terms; PP entropy on dnc has lbinom args
// ~4e5 so absolute drift hits ~1e-9, relative ~3e-14.
const REL_S_TOL = 1e-12;
const ABS_dS_TOL = 1e-7;
function withinRelS(drift, S) {
  return drift <= REL_S_TOL * Math.max(1, Math.abs(S));
}

const renum = trace.renum_to_orig;
const N = renum.length;
const edges = loadEdges(edgePath, renum);
const G = COMDET.SBM.Graph(N, edges, { correctSelfLoops: false });

// Replay one sweep against `state`: feed (toS, accept) from `lvl.visits`
// through opts.proposalOracle + opts.visitOrder, validate dS / cands /
// fromR / toS / accept / moved per-visit byte-equal vs cpp, and verify
// state.entropy() matches lvl.S_post within REL_S_TOL.
const RNG_STUB = COMDET.SBM.MT19937(0);
function replaySweep(state, lvl, beta, sweepLabel, agg) {
  const oracle = (v, i) => {
    const t = lvl.visits[i];
    return { to_block: t.toS, accept: t.accept };
  };
  const out = COMDET.SBM.mcmcSweep(state, RNG_STUB, {
    visitOrder: lvl.visit_order,
    proposalOracle: oracle,
    recordTrace: true,
    beta: beta,
  });
  if (out.traces.length !== lvl.visits.length) {
    agg.failures.push(`${sweepLabel}: visit count mismatch js=${out.traces.length} cpp=${lvl.visits.length}`);
    return;
  }
  for (let i = 0; i < lvl.visits.length; i++) {
    const t = lvl.visits[i];
    const j = out.traces[i];
    agg.visits++;
    if (j.v !== t.v) agg.failures.push(`${sweepLabel} visit ${i}: v ${j.v} != ${t.v}`);
    if (j.fromR !== t.fromR) agg.fromR++;
    if (j.toS !== t.toS) agg.toS++;
    if (j.accept !== t.accept) agg.accept++;
    if (j.accepted !== t.moved) agg.moved++;
    let candDiff = j.cands.length !== t.cands.length;
    if (!candDiff) {
      for (let k = 0; k < j.cands.length; k++) if (j.cands[k] !== t.cands[k]) { candDiff = true; break; }
    }
    if (candDiff) agg.cands++;
    const d = Math.abs(j.dS - t.dS);
    if (d > agg.dSMax) agg.dSMax = d;
  }
  const S_post_js = state.entropy();
  const S_drift = Math.abs(S_post_js - lvl.S_post);
  if (S_drift > agg.SMax) agg.SMax = S_drift;
  if (!withinRelS(S_drift, lvl.S_post)) {
    agg.failures.push(`${sweepLabel}: S_post drift ${S_drift.toExponential(3)} (js=${S_post_js}, cpp=${lvl.S_post})`);
  }
}

function newAgg() {
  return {
    failures: [], visits: 0, dSMax: 0, SMax: 0,
    fromR: 0, toS: 0, accept: 0, moved: 0, cands: 0,
  };
}

function reportAgg(agg, totalSize, label) {
  if (agg.fromR) agg.failures.push(`fromR mismatches: ${agg.fromR}/${agg.visits}`);
  if (agg.toS) agg.failures.push(`toS mismatches: ${agg.toS}/${agg.visits}`);
  if (agg.cands) agg.failures.push(`cand-pool mismatches: ${agg.cands}/${agg.visits}`);
  if (agg.accept) agg.failures.push(`accept mismatches: ${agg.accept}/${agg.visits}`);
  if (agg.moved) agg.failures.push(`moved mismatches: ${agg.moved}/${agg.visits}`);
  if (agg.dSMax > ABS_dS_TOL) agg.failures.push(`dS max drift ${agg.dSMax.toExponential(3)} > ${ABS_dS_TOL}`);
  if (agg.failures.length > 0) {
    console.error("FAIL:");
    for (const f of agg.failures) console.error("  " + f);
    process.exit(1);
  }
  console.log(`PASS: ${label} (${totalSize}, ${trace.sweeps.length} sweeps).`);
}

if (nestedFlag) {
  // Nested replay: per-level state stack mirroring runNested in cpp.
  const hier = trace.hierarchy.map((arr) => Int32Array.from(arr));
  const graphs = [G];
  const states = [COMDET.SBM.BlockState(G, { mode: mode, init: hier[0] })];
  for (let l = 1; l < hier.length; l++) {
    const built = COMDET.SBM.buildLevelGraph(graphs[l - 1], states[l - 1], hier[l]);
    graphs.push(built.graph);
    states.push(COMDET.SBM.BlockState(built.graph, { mode: "ndc", init: built.init }));
  }
  function totalS() { let S = 0; for (const st of states) S += st.entropy(); return S; }

  const agg = newAgg();
  const S_init_drift = Math.abs(totalS() - trace.S_init);
  if (!withinRelS(S_init_drift, trace.S_init))
    agg.failures.push(`total S_init drift ${S_init_drift.toExponential(3)}`);
  for (let l = 0; l < states.length; l++) {
    const sl_js = states[l].entropy();
    const d = Math.abs(sl_js - trace.level_S_init[l]);
    if (!withinRelS(d, trace.level_S_init[l]))
      agg.failures.push(`level ${l} S_init drift ${d.toExponential(3)}`);
  }

  for (let sw = 0; sw < trace.sweeps.length; sw++) {
    const swTrace = trace.sweeps[sw];
    for (let li = 0; li < swTrace.levels.length; li++) {
      const levelEntry = swTrace.levels[li];
      // [TRACE-SBM-REBUILD] format change 2026-05-10: nested level
      // entries now wrap the runSweep payload in `sweep_payload` so a
      // sibling `rebuilds` array can ride alongside. Accept both old
      // (raw payload) and new (wrapped) traces.
      const lvl = levelEntry.sweep_payload || levelEntry;
      const l = lvl.level;
      replaySweep(states[l], lvl, trace.beta, `sweep ${sw} level ${l}`, agg);
      // Propagate updated e_rs to upper levels (mirrors cpp's runNested).
      for (let up = l + 1; up < states.length; up++) {
        const built = COMDET.SBM.buildLevelGraph(graphs[up - 1], states[up - 1], hier[up]);
        graphs[up] = built.graph;
        states[up] = COMDET.SBM.BlockState(built.graph, { mode: "ndc", init: built.init });
      }
    }
    const S_sw_drift = Math.abs(totalS() - swTrace.S_post);
    if (!withinRelS(S_sw_drift, swTrace.S_post))
      agg.failures.push(`sweep ${sw}: total S_post drift ${S_sw_drift.toExponential(3)}`);
  }

  let memDiffs = 0;
  for (let l = 0; l < states.length; l++) {
    const m_js = states[l].blockMembership();
    const m_cpp = trace.level_final_membership[l];
    if (m_js.length !== m_cpp.length) {
      agg.failures.push(`level ${l} membership length mismatch`);
      continue;
    }
    for (let v = 0; v < m_js.length; v++) if (m_js[v] !== m_cpp[v]) memDiffs++;
  }
  if (memDiffs > 0) agg.failures.push(`final per-level membership ${memDiffs} entries differ`);

  console.log(`=== SBM nested-${mode} replay ===`);
  console.log(`levels=${states.length} n=${N} sweeps=${trace.sweeps.length} visits=${agg.visits}`);
  console.log(`total S: js=${totalS().toFixed(15)} cpp=${trace.S_final.toFixed(15)}`);
  console.log(`max dS drift = ${agg.dSMax.toExponential(3)}  max S drift = ${agg.SMax.toExponential(3)}`);
  console.log(`final per-level membership diffs = ${memDiffs}`);
  reportAgg(agg, `${states.length} levels`, "byte-equal nested trace + final per-level partition");
} else {
  // Flat replay: single BlockState; sweeps come keyed by sweep index.
  const state = COMDET.SBM.BlockState(G, { mode: mode, init: trace.init_membership });

  const agg = newAgg();
  const S_init_drift = Math.abs(state.entropy() - trace.S_init);
  if (!withinRelS(S_init_drift, trace.S_init))
    agg.failures.push(`S_init drift ${S_init_drift.toExponential(3)}`);

  for (let sw = 0; sw < trace.sweeps.length; sw++) {
    replaySweep(state, trace.sweeps[sw], trace.beta, `sweep ${sw}`, agg);
  }

  const finalJs = state.blockMembership();
  let memDiffs = 0;
  for (let v = 0; v < N; v++) if (finalJs[v] !== trace.final_membership[v]) memDiffs++;
  if (memDiffs > 0) agg.failures.push(`final_membership ${memDiffs}/${N} entries differ`);

  console.log(`=== SBM flat-${mode} replay ===`);
  console.log(`n=${N} sweeps=${trace.sweeps.length} visits=${agg.visits}`);
  console.log(`S_init: js=${state.entropy().toFixed(15)} cpp=${trace.S_init.toFixed(15)} drift=${S_init_drift.toExponential(3)}`);
  console.log(`S_final: cpp=${trace.S_final.toFixed(15)}`);
  console.log(`max dS drift = ${agg.dSMax.toExponential(3)}  max S drift = ${agg.SMax.toExponential(3)}`);
  console.log(`final_membership diffs = ${memDiffs}`);
  reportAgg(agg, `n=${N}`, "byte-equal trace + final partition");
}
