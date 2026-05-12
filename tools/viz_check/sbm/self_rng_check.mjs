// SBM self-RNG byte-equal check.
//
// Runs JS COMDET.SBM.mcmcSweep WITHOUT proposalOracle / visitOrder, seeded
// with the same value as the cpp tracer. Asserts:
//   - S_init bit-equal vs cpp.
//   - Per-visit (v, toS, pickIdx, dS, subsetSE_before/after,
//     acceptU, expArg, expVal, accept) bit-equal vs cpp.
//   - Per-sweep S_post bit-equal vs cpp.
//   - Per-sweep S_post_breakdown component-wise bit-equal vs cpp.
//   - Final per-block membership byte-equal vs cpp.
//
// Per-visit probes mirror cpp flat_traced.cpp `[TRACE-SBM-PICKIDX]` /
// `[TRACE-SBM-SUBSETSE]` / `[TRACE-SBM-ACCEPT-FP]` emit sites. Per-sweep
// breakdown mirrors `[TRACE-SBM-BRK]`. Closes gap-audit P0 items 1, 2,
// 3, 5 + (nested) items 6, 8, 10, 11 — see
// memory/community-detection/sbm/tracer_coverage_gaps.md.
//
// This is the random/deterministic separation's "self-random" leg: JS uses
// its own MT19937 + intRange + shuffle. cpp tracer uses a matching JSRng
// (mt19937 + identical rejection-mod intRange + same shuffle). Both are
// byte-equal RNG implementations, so JS and cpp consume the exact same
// RNG sequence and produce byte-equal traces.
//
// Usage: node self_rng_check.mjs <trace.json> <edge.csv> <mode> [--nested]

import fs from "node:fs";
import path from "node:path";

const tracePath = process.argv[2];
const edgePath = process.argv[3];
const mode = process.argv[4];
const nestedFlag = process.argv.includes("--nested");

// cpp tracer emits NaN / +/-Infinity as the JSON string tokens
// "NaN" / "Infinity" / "-Infinity" (default `operator<<` on NaN
// writes literal `nan` which is NOT valid JSON). Round-trip them
// back to the corresponding IEEE-754 doubles via this reviver so
// `bitsOf(...)` sees the same NaN bit pattern on both sides. The
// nested tracer hits this when candidatePool returns a fresh-block
// target s == B (capacity), routing the OOB-tolerant `ers_at` in
// cpp + the JS Float64Array undefined-coerce path to NaN dS /
// subsetSE / expArg / expVal.
function nanReviver(_k, v) {
  if (v === "NaN") return NaN;
  if (v === "Infinity") return Infinity;
  if (v === "-Infinity") return -Infinity;
  return v;
}
const trace = JSON.parse(fs.readFileSync(tracePath, "utf8"), nanReviver);

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = "/home/vltanh/Documents/web/vltanh.github.io/comdet/js";
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
    edges.push([orig_to_new.get(cols[0]), orig_to_new.get(cols[1])]);
  }
  return edges;
}

function bitsOf(x) {
  const b = new Float64Array(1); b[0] = x;
  return new BigUint64Array(b.buffer)[0];
}

// [TRACE-SBM-BRK] component-wise breakdown bit-compare. Returns count
// of bit-mismatching summands (max 7).
function diffBreakdown(jsBrk, cppBrk) {
  let n = 0;
  for (const k of ["exact", "edges_dl", "dc_deg_const",
                   "partition_dl", "degree_dl", "pp_lik", "pp_edges"]) {
    if (bitsOf(jsBrk[k] || 0) !== bitsOf(cppBrk[k] || 0)) n++;
  }
  return n;
}

// [TRACE-SBM-REBUILD] post-rebuild dump bit-compare. Verifies each
// upper-level rebuilt graph matches cpp on N + E + nonEmpty[] + init[]
// + strength[] + post-rebuild Bne + S + breakdown (closes gap-audit
// items 6, 7, 10).
function diffRebuild(jsR, cppR) {
  let m = 0;
  if (jsR.N !== cppR.N) m++;
  if (bitsOf(jsR.E) !== bitsOf(cppR.E)) m++;
  if (jsR.nonEmpty.length !== cppR.nonEmpty.length) m++;
  else for (let i = 0; i < jsR.nonEmpty.length; i++)
    if (jsR.nonEmpty[i] !== cppR.nonEmpty[i]) { m++; break; }
  if (jsR.init.length !== cppR.init.length) m++;
  else for (let i = 0; i < jsR.init.length; i++)
    if (jsR.init[i] !== cppR.init[i]) { m++; break; }
  if (jsR.strength.length !== cppR.strength.length) m++;
  else for (let i = 0; i < jsR.strength.length; i++)
    if (bitsOf(jsR.strength[i]) !== bitsOf(cppR.strength[i])) { m++; break; }
  if (jsR.Bne_post_rebuild !== cppR.Bne_post_rebuild) m++;
  if (bitsOf(jsR.S_post_rebuild) !== bitsOf(cppR.S_post_rebuild)) m++;
  return m;
}

const renum = trace.renum_to_orig;
const N = renum.length;
const edges = loadEdges(edgePath, renum);
const G = COMDET.SBM.Graph(N, edges, { correctSelfLoops: false });
const seed = trace.seed != null ? (trace.seed >>> 0) : 7;

function checkFlat() {
  const state = COMDET.SBM.BlockState(G, { mode, init: trace.init_membership });
  const RNG = COMDET.SBM.MT19937(seed);

  const sj = state.entropy(), sc = trace.S_init;
  const sjBits = bitsOf(sj), scBits = bitsOf(sc);
  const sInitOk = sjBits === scBits;
  console.log(`S_init: js=${sj} cpp=${sc} bits-equal=${sInitOk}`);
  if (!sInitOk) process.exit(1);

  let visits = 0, dsMm = 0, toMm = 0, accMm = 0, voMm = 0, spostMm = 0;
  let pickMm = 0, subBefMm = 0, subAftMm = 0, auMm = 0, expArgMm = 0, expValMm = 0;
  let brkMm = 0;
  // [TRACE-SBM-DRAWS] gap-audit item 4 (row B). Per-visit raw() delta
  // bit-compare. cpp emits `rng_draws` per visit; JS records its own
  // `rng_draws` via rng.drawCount() in mcmc.js when `recordProbes` is on.
  let drawMm = 0;
  for (let sw = 0; sw < trace.sweeps.length; sw++) {
    const swT = trace.sweeps[sw];
    const out = COMDET.SBM.mcmcSweep(state, RNG, {
      recordTrace: true, recordProbes: true, beta: trace.beta,
    });
    if (out.traces.length !== swT.visits.length) {
      console.error(`sweep ${sw}: visits length mismatch`);
      process.exit(1);
    }
    for (let i = 0; i < swT.visits.length; i++) {
      visits++;
      const t = swT.visits[i], j = out.traces[i];
      if (j.v !== t.v) voMm++;
      if (j.toS !== t.toS) toMm++;
      if (j.pickIdx !== t.pickIdx) pickMm++;
      if (bitsOf(j.dS) !== bitsOf(t.dS)) dsMm++;
      if (bitsOf(j.subsetSE_before) !== bitsOf(t.subsetSE_before)) subBefMm++;
      if (bitsOf(j.subsetSE_after)  !== bitsOf(t.subsetSE_after))  subAftMm++;
      if (bitsOf(j.acceptU) !== bitsOf(t.acceptU)) auMm++;
      if (bitsOf(j.expArg)  !== bitsOf(t.expArg))  expArgMm++;
      if (bitsOf(j.expVal)  !== bitsOf(t.expVal))  expValMm++;
      if (j.accept !== t.accept) accMm++;
      if (t.rng_draws !== undefined && j.rng_draws !== t.rng_draws) drawMm++;
    }
    const sjp = state.entropy(), scp = swT.S_post;
    if (bitsOf(sjp) !== bitsOf(scp)) spostMm++;
    // [TRACE-SBM-BRK] per-sweep breakdown bit-compare.
    if (swT.S_post_breakdown) {
      const jsBrk = state.entropyBreakdown();
      brkMm += diffBreakdown(jsBrk, swT.S_post_breakdown);
    }
  }
  console.log(
    `flat ${mode}: visits=${visits} ` +
    `visit-order-mismatches=${voMm} ` +
    `toS-mismatches=${toMm} ` +
    `pickIdx-mismatches=${pickMm} ` +
    `dS-bit-mismatches=${dsMm} ` +
    `subsetSE_before-bit-mismatches=${subBefMm} ` +
    `subsetSE_after-bit-mismatches=${subAftMm} ` +
    `acceptU-bit-mismatches=${auMm} ` +
    `expArg-bit-mismatches=${expArgMm} ` +
    `expVal-bit-mismatches=${expValMm} ` +
    `accept-mismatches=${accMm} ` +
    `rng_draws-mismatches=${drawMm} ` +
    `S_post-bit-mismatches=${spostMm}/${trace.sweeps.length} ` +
    `S_post_breakdown-summand-mismatches=${brkMm}`);
  const total = voMm + toMm + pickMm + dsMm + subBefMm + subAftMm
              + auMm + expArgMm + expValMm + accMm + drawMm + spostMm + brkMm;
  if (total > 0) process.exit(1);
}

function checkNested() {
  const hier = trace.hierarchy.map(a => Int32Array.from(a));
  const graphs = [G];
  const states = [COMDET.SBM.BlockState(G, { mode, init: hier[0] })];
  for (let l = 1; l < hier.length; l++) {
    const built = COMDET.SBM.buildLevelGraph(graphs[l-1], states[l-1], hier[l]);
    graphs.push(built.graph);
    states.push(COMDET.SBM.BlockState(built.graph, { mode: "ndc", init: built.init }));
  }
  const RNG = COMDET.SBM.MT19937(seed);
  let visits = 0, dsMm = 0, toMm = 0, accMm = 0, voMm = 0;
  let pickMm = 0, subBefMm = 0, subAftMm = 0, auMm = 0, expArgMm = 0, expValMm = 0;
  // [TRACE-SBM-DRAWS] per-visit raw() delta bit-compare on nested.
  let drawMm = 0;
  // [TRACE-SBM-LEVEL] + [TRACE-SBM-REBUILD] nested-side bit-compare.
  // Closes gap-audit items 6, 8, 11 (level S_post per sweep + rebuild
  // dumps bit-equal vs cpp).
  let lvlSPostMm = 0, rebuildMm = 0, sPostMm = 0, brkMm = 0;
  // [TRACE-SBM-LEVEL-MEMB] gap-audit item 9: per-sweep level membership
  // bit-compare. Surfaces divergence at sweep k>0 on level l>0 that the
  // end-state `level_final_membership` only catches transitively.
  let lvlMembMm = 0;
  for (let sw = 0; sw < trace.sweeps.length; sw++) {
    const swT = trace.sweeps[sw];
    for (let li = 0; li < swT.levels.length; li++) {
      const levelEntry = swT.levels[li];
      // Backward-compat: older traces stored runSweep payload directly
      // at the level entry; new traces wrap in `sweep_payload` so a
      // sibling `rebuilds` array can ride alongside. Accept both.
      const lvl = levelEntry.sweep_payload || levelEntry;
      const l = lvl.level;
      const out = COMDET.SBM.mcmcSweep(states[l], RNG, {
        recordTrace: true, recordProbes: true, beta: trace.beta,
      });
      if (out.traces.length !== lvl.visits.length) {
        console.error(`sweep ${sw} level ${l}: visits length mismatch`);
        process.exit(1);
      }
      for (let i = 0; i < lvl.visits.length; i++) {
        visits++;
        const t = lvl.visits[i], j = out.traces[i];
        if (j.v !== t.v) voMm++;
        if (j.toS !== t.toS) toMm++;
        if (j.pickIdx !== t.pickIdx) pickMm++;
        if (bitsOf(j.dS) !== bitsOf(t.dS)) dsMm++;
        if (bitsOf(j.subsetSE_before) !== bitsOf(t.subsetSE_before)) subBefMm++;
        if (bitsOf(j.subsetSE_after)  !== bitsOf(t.subsetSE_after))  subAftMm++;
        if (bitsOf(j.acceptU) !== bitsOf(t.acceptU)) auMm++;
        if (bitsOf(j.expArg)  !== bitsOf(t.expArg))  expArgMm++;
        if (bitsOf(j.expVal)  !== bitsOf(t.expVal))  expValMm++;
        if (j.accept !== t.accept) accMm++;
        if (t.rng_draws !== undefined && j.rng_draws !== t.rng_draws) drawMm++;
      }
      // [TRACE-SBM-LEVEL] / [TRACE-SBM-BRK]: per-level S_post + breakdown
      // immediately after this level's sweep (before rebuilds).
      const sjLevel = states[l].entropy();
      if (bitsOf(sjLevel) !== bitsOf(lvl.S_post)) sPostMm++;
      if (lvl.S_post_breakdown) {
        brkMm += diffBreakdown(states[l].entropyBreakdown(),
                               lvl.S_post_breakdown);
      }
      // [TRACE-SBM-REBUILD]: rebuild dump compare for each up-level.
      const rebuilds = levelEntry.rebuilds || [];
      for (let ri = 0; ri < rebuilds.length; ri++) {
        const cppR = rebuilds[ri];
        const up = cppR.up;
        const built = COMDET.SBM.buildLevelGraph(graphs[up-1], states[up-1], hier[up]);
        graphs[up] = built.graph;
        states[up] = COMDET.SBM.BlockState(built.graph, { mode: "ndc", init: built.init });
        // Collect probe data from the rebuilt graph + fresh state.
        const ne = states[up-1].nonEmptyBlocks();
        // graph.totalEdgeWeight() = sum of edge weights = cpp's g.E
        // accumulator. Mirrors block_state.js:30 reading.
        const E = built.graph.totalEdgeWeight();
        const strength = new Array(built.graph.vcount());
        for (let i = 0; i < strength.length; i++)
          strength[i] = built.graph.strength(i);
        const initArr = Array.from(built.init);
        const nonEmptyArr = Array.from(ne);
        const jsR = {
          N: built.graph.vcount(), E: E,
          nonEmpty: nonEmptyArr, init: initArr, strength: strength,
          Bne_post_rebuild: states[up].nonEmptyBlocks().length,
          S_post_rebuild: states[up].entropy(),
        };
        rebuildMm += diffRebuild(jsR, cppR);
      }
    }
    // [TRACE-SBM-LEVEL]: per-sweep level_S_post array.
    if (swT.level_S_post) {
      for (let l = 0; l < swT.level_S_post.length; l++) {
        if (bitsOf(states[l].entropy()) !== bitsOf(swT.level_S_post[l]))
          lvlSPostMm++;
      }
    }
    // [TRACE-SBM-LEVEL-MEMB]: per-sweep level membership bit-compare.
    if (swT.level_membership) {
      for (let l = 0; l < swT.level_membership.length; l++) {
        const cppMb = swT.level_membership[l];
        for (let v = 0; v < cppMb.length; v++) {
          if (states[l].blockOf(v) !== cppMb[v]) { lvlMembMm++; break; }
        }
      }
    }
  }
  console.log(
    `nested ${mode}: visits=${visits} ` +
    `visit-order-mismatches=${voMm} ` +
    `toS-mismatches=${toMm} ` +
    `pickIdx-mismatches=${pickMm} ` +
    `dS-bit-mismatches=${dsMm} ` +
    `subsetSE_before-bit-mismatches=${subBefMm} ` +
    `subsetSE_after-bit-mismatches=${subAftMm} ` +
    `acceptU-bit-mismatches=${auMm} ` +
    `expArg-bit-mismatches=${expArgMm} ` +
    `expVal-bit-mismatches=${expValMm} ` +
    `accept-mismatches=${accMm} ` +
    `rng_draws-mismatches=${drawMm} ` +
    `level_S_post-bit-mismatches=${lvlSPostMm} ` +
    `level_membership-mismatches=${lvlMembMm} ` +
    `S_post-immediate-bit-mismatches=${sPostMm} ` +
    `S_post_breakdown-summand-mismatches=${brkMm} ` +
    `rebuild-field-mismatches=${rebuildMm}`);
  const total = voMm + toMm + pickMm + dsMm + subBefMm + subAftMm
              + auMm + expArgMm + expValMm + accMm + drawMm
              + lvlSPostMm + lvlMembMm + sPostMm + brkMm + rebuildMm;
  if (total > 0) process.exit(1);
}

if (nestedFlag) checkNested();
else checkFlat();
