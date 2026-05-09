// SBM self-RNG byte-equal check.
//
// Runs JS COMDET.SBM.mcmcSweep WITHOUT proposalOracle / visitOrder, seeded
// with the same value as the cpp tracer. Asserts:
//   - S_init bit-equal vs cpp.
//   - Per-visit (toS, dS, accept) bit-equal vs cpp.
//   - Per-sweep S_post bit-equal vs cpp.
//   - Final per-block membership byte-equal vs cpp.
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

const trace = JSON.parse(fs.readFileSync(tracePath, "utf8"));

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
  for (let sw = 0; sw < trace.sweeps.length; sw++) {
    const swT = trace.sweeps[sw];
    const out = COMDET.SBM.mcmcSweep(state, RNG, {
      recordTrace: true, beta: trace.beta,
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
      if (bitsOf(j.dS) !== bitsOf(t.dS)) dsMm++;
      if (j.accept !== t.accept) accMm++;
    }
    const sjp = state.entropy(), scp = swT.S_post;
    if (bitsOf(sjp) !== bitsOf(scp)) spostMm++;
  }
  console.log(
    `flat ${mode}: visits=${visits} ` +
    `visit-order-mismatches=${voMm} ` +
    `toS-mismatches=${toMm} ` +
    `dS-bit-mismatches=${dsMm} ` +
    `accept-mismatches=${accMm} ` +
    `S_post-bit-mismatches=${spostMm}/${trace.sweeps.length}`);
  const total = voMm + toMm + dsMm + accMm + spostMm;
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
  for (let sw = 0; sw < trace.sweeps.length; sw++) {
    const swT = trace.sweeps[sw];
    for (let li = 0; li < swT.levels.length; li++) {
      const lvl = swT.levels[li], l = lvl.level;
      const out = COMDET.SBM.mcmcSweep(states[l], RNG, {
        recordTrace: true, beta: trace.beta,
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
        if (bitsOf(j.dS) !== bitsOf(t.dS)) dsMm++;
        if (j.accept !== t.accept) accMm++;
      }
      for (let up = l + 1; up < states.length; up++) {
        const built = COMDET.SBM.buildLevelGraph(graphs[up-1], states[up-1], hier[up]);
        graphs[up] = built.graph;
        states[up] = COMDET.SBM.BlockState(built.graph, { mode: "ndc", init: built.init });
      }
    }
  }
  console.log(
    `nested ${mode}: visits=${visits} ` +
    `visit-order-mismatches=${voMm} ` +
    `toS-mismatches=${toMm} ` +
    `dS-bit-mismatches=${dsMm} ` +
    `accept-mismatches=${accMm}`);
  if (voMm + toMm + dsMm + accMm > 0) process.exit(1);
}

if (nestedFlag) checkNested();
else checkFlat();
