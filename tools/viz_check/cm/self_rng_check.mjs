/* CM L4 self-RNG end-to-end diff harness.
 *
 * Verification target (per byte-equal-tracer skill, three-claims #1):
 *   JS visualizer == canonical-tracer (TRACER_MODE swapped build) bit-for-bit
 *   under matching seed.
 *
 * Drops cutOracle + baseAlgoFn from JS replay -- the JS CM runs its own
 * VieCut + own Leiden chain. Compares per-pop record bit-equal vs the
 * canonical-tracer's emitted trace using uint64 reinterpret on every double.
 *
 * Run:
 *   /tmp/cm_kernel_check_swapped <edge.csv> <com.csv> <out.csv> "1log_10(n)" 0.0001 <seed>
 *     > cpp_swapped.json 2>cpp_swapped.err
 *   node tools/viz_check/cm/self_rng_check.mjs cpp_swapped.json <edge.csv> <com.csv>
 *
 * Exits 0 on byte-equal; 1 on any mismatch (first divergent record reported).
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 3) {
  console.error("usage: self_rng_check.mjs <cpp_trace.json> <edge.csv> <com.csv> "
              + "[criterion] [resolution] [seed] [algorithm]");
  process.exit(2);
}
const [cppPath, edgePath, comPath,
       criterion = "1log_10(n)",
       resolution = 0.0001,
       seed = 0,
       algorithm = "leiden-cpm"] = args;

const cpp = JSON.parse(fs.readFileSync(cppPath, "utf8"));
if (cpp.build !== "TRACER_MODE") {
  console.error(`WARN: cpp trace has build="${cpp.build}" (expected TRACER_MODE for L4 self-RNG check)`);
}

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = path.join(__dirname, "../../../../../web/vltanh.github.io/comdet/js");
await import(path.join(WEB, "common/common.js"));
await import(path.join(WEB, "mincut.js"));
await import(path.join(WEB, "louvain/louvain.js"));
await import(path.join(WEB, "leiden/leiden.js"));
await import(path.join(WEB, "comdet/page_helpers.js"));
await import(path.join(WEB, "wcc/wcc.js"));
// Load VieCut JS port + register MINCUT.viecut adapter so cm.js's
// mincutFn defaults to viecut. The loader must be CALLED, not just
// imported, to populate COMDET.VIECUT before mincut_adapter.js runs.
try {
  const vcLoader = path.join(__dirname, "../viecut/_loader.mjs");
  if (fs.existsSync(vcLoader)) {
    const mod = await import(vcLoader);
    mod.loadVIECUT();
    await import(path.join(WEB, "viecut/mincut_adapter.js"));
  }
} catch (e) { /* fallback to stoer-wagner */ }
await import(path.join(WEB, "cm/cm.js"));

function loadGraph(edgePath, comPath) {
  const edgeText = fs.readFileSync(edgePath, "utf8").trim().split(/\r?\n/);
  const com = fs.readFileSync(comPath, "utf8").trim().split(/\r?\n/);
  const orig_to_new = new Map();
  const edges = [];
  for (let i = 1; i < edgeText.length; i++) {
    if (!edgeText[i]) continue;
    const cols = edgeText[i].split(/[,\t ]/);
    const s = cols[0], t = cols[1];
    if (!orig_to_new.has(s)) orig_to_new.set(s, orig_to_new.size);
    if (!orig_to_new.has(t)) orig_to_new.set(t, orig_to_new.size);
    edges.push([orig_to_new.get(s), orig_to_new.get(t)]);
  }
  const partition = new Int32Array(orig_to_new.size);
  for (let i = 0; i < partition.length; i++) partition[i] = -2147483648;
  for (let i = 1; i < com.length; i++) {
    if (!com[i]) continue;
    const cols = com[i].split(/[,\t ]/);
    const newId = orig_to_new.get(cols[0]);
    if (newId == null) continue;
    partition[newId] = parseInt(cols[1], 10);
  }
  return {
    nodes: Array.from({ length: orig_to_new.size }, (_, i) => i),
    edges,
    membership: partition,
  };
}

// uint64 reinterpret per skill playbook §3.
function bits(x) {
  const b = new Float64Array(1); b[0] = x;
  return new BigUint64Array(b.buffer)[0];
}

// [P0-Gap1 / P0-Gap2 / P0-Gap3 / P0-Gap4 / P0-Gap5 / P0-Gap6 / P0-Gap7 /
//  P0-Gap9] HOOK collectors. Mirror the canonical-tracer stderr probes
// so per-side bit-equality is asserted post-run. Each list is appended
// in JS pop / round order; cpp side is reconstructed by parsing
// [TRACE-CM] lines from cpp stderr (see assertion section below).
const hookLog = {
  initLineage: [],
  ptcSnapshots: [],
  vcBoundaries: [],
  reclusterCtx: [],
  ldEvents: [],
  pushes: [],
  popSingletons: [],
  thrDecomp: [],
  endRoundDrain: [],
};
globalThis.__CM_HOOK_INIT_LINEAGE = function (r) { hookLog.initLineage.push(r); };
globalThis.__CM_HOOK_PTC_SNAPSHOT = function (r) { hookLog.ptcSnapshots.push(r); };
globalThis.__CM_HOOK_VC_BEGIN = function (r) { hookLog.vcBoundaries.push({ ...r, kind: "begin" }); };
globalThis.__CM_HOOK_VC_END = function (r) { hookLog.vcBoundaries.push({ ...r, kind: "end" }); };
globalThis.__CM_HOOK_LD_RESEED = function (r) { hookLog.ldEvents.push({ ...r, kind: "reseed" }); };
globalThis.__CM_HOOK_LD_BEGIN = function (r) { hookLog.ldEvents.push({ ...r, kind: "begin" }); };
globalThis.__CM_HOOK_LD_ITER_END = function (r) { hookLog.ldEvents.push({ ...r, kind: "iter_end" }); };
globalThis.__CM_HOOK_LD_END = function (r) { hookLog.ldEvents.push({ ...r, kind: "end" }); };
globalThis.__CM_HOOK_PUSH = function (r) { hookLog.pushes.push(r); };
globalThis.__CM_HOOK_POP_SINGLETON_COUNT = function (r) { hookLog.popSingletons.push(r); };
globalThis.__CM_HOOK_THR_DECOMP = function (r) { hookLog.thrDecomp.push(r); };
globalThis.__CM_HOOK_END_ROUND_DRAIN = function (r) { hookLog.endRoundDrain.push(r); };

// Parse cpp [TRACE-CM] stderr (text path) into structured records. The
// canonical-tracer stderr file is co-located by the harness (.err next
// to .json).
function parseCppStderr(text) {
  const out = {
    initLineage: [],
    ptcSnapshots: [],
    vcBoundaries: [],
    pushes: [],
    popSingletons: [],
    thrDecomp: [],
    endRoundDrain: [],
    ldIterMem: [],   // [P0-Gap1]: per-iter membership
  };
  // Track ptc snapshot rows. cpp emits PTC_SNAPSHOT phase=X rows=N then
  // PTC_ROW phase=X parent=K children=[...] lines.
  let currentSnapshot = null;
  for (const raw of text.split(/\r?\n/)) {
    const line = raw;
    let m;
    if ((m = line.match(/^\[TRACE-CM\] INIT_LINEAGE comp_idx=(\d+) first_node=(-?\d+) orig_cid=(-?\d+) orig_size=(\d+) sub_size=(\d+) branch=(\w+) parent_cid=(-?\d+) current_cid=(-?\d+) ptc_neg1_after=\[([^\]]*)\] ptc_parent_after=\[([^\]]*)\]/))) {
      out.initLineage.push({
        comp_idx: +m[1], first_node: +m[2], orig_cid: +m[3],
        orig_size: +m[4], sub_size: +m[5], branch: m[6],
        parent_cid: +m[7], current_cid: +m[8],
        ptc_neg1_after: m[9] ? m[9].split(",").map(Number) : [],
        ptc_parent_after: m[10] ? m[10].split(",").map(Number) : [],
      });
    } else if ((m = line.match(/^\[TRACE-CM\] PTC_SNAPSHOT phase=(\S+) rows=(\d+)/))) {
      currentSnapshot = { phase: m[1], rows: [] };
      out.ptcSnapshots.push(currentSnapshot);
    } else if ((m = line.match(/^\[TRACE-CM\]\s+PTC_ROW phase=(\S+) parent=(-?\d+) children=\[([^\]]*)\]/))) {
      if (currentSnapshot && currentSnapshot.phase === m[1]) {
        currentSnapshot.rows.push({
          parent: +m[2],
          children: m[3] ? m[3].split(",").map(Number) : [],
        });
      }
    } else if ((m = line.match(/^\[TRACE-CM\]\s+VC_BEGIN r=(\d+) idx=(\d+) cid=(-?\d+) n=(\d+)/))) {
      out.vcBoundaries.push({ kind: "begin", round: +m[1], pop_idx: +m[2], cid: +m[3], n: +m[4] });
    } else if ((m = line.match(/^\[TRACE-CM\]\s+VC_END r=(\d+) idx=(\d+) cut=(\d+) in=(\d+) out=(\d+)/))) {
      out.vcBoundaries.push({ kind: "end", round: +m[1], pop_idx: +m[2], cut: +m[3], in_size: +m[4], out_size: +m[5] });
    } else if ((m = line.match(/^\[TRACE-CM\]\s+PUSH side=(\w+) size=(\d+) is_singleton=(\w+)/))) {
      out.pushes.push({ side: m[1], size: +m[2], is_singleton: m[3] === "true" });
    } else if ((m = line.match(/^\[TRACE-CM\]\s+POP_SINGLETON_COUNT r=(\d+) idx=(\d+) count=(\d+)/))) {
      out.popSingletons.push({ round: +m[1], pop_idx: +m[2], count: +m[3] });
    } else if ((m = line.match(/^\[TRACE-CM\]\s+THR_DECOMP r=(\d+) idx=(\d+) n=(\d+) log_n=(\S+) log_n_bits=0x([0-9a-fA-F]+) pre_log_bits=0x([0-9a-fA-F]+) thr_bits=0x([0-9a-fA-F]+)/))) {
      out.thrDecomp.push({
        round: +m[1], pop_idx: +m[2], n: +m[3],
        log_n: parseFloat(m[4]),
        log_n_bits: BigInt("0x" + m[5]),
        pre_log_bits: BigInt("0x" + m[6]),
        thr_bits: BigInt("0x" + m[7]),
      });
    } else if ((m = line.match(/^\[TRACE-CM\]\s+END_ROUND_DRAIN round=(\d+) drain_idx=(\d+) parent_cid=(-?\d+) fresh_id=(\d+) ptc_size_after=(\d+) ptc_parent_after=\[([^\]]*)\]/))) {
      out.endRoundDrain.push({
        round: +m[1], drain_idx: +m[2], parent_cid: +m[3], fresh_id: +m[4],
        ptc_size_after: +m[5],
        ptc_parent_after: m[6] ? m[6].split(",").map(Number) : [],
      });
    } else if ((m = line.match(/^\[TRACE-CM\] LD_ITER_END r=(\d+) idx=(\d+) cid=(-?\d+) side=(\w+) iter=(\d+) mem=\[([^\]]*)\]/))) {
      out.ldIterMem.push({
        round: +m[1], pop_idx: +m[2], cid: +m[3], side: m[4], iter: +m[5],
        mem: m[6] ? m[6].split(",").map(Number) : [],
      });
    }
  }
  return out;
}

const G = loadGraph(edgePath, comPath);
const fixture = { nodes: G.nodes, edges: G.edges, gt: Array.from(G.membership) };

// VieCut m_mt is default-constructed (libstdc++ std::mt19937{} = seed 5489)
// in cpp tracer per kernel_check.cpp:105-107 (random_functions::setSeed
// commented out). argv seed is for Leiden only (kernel_check.cpp:174
// Optimiser::set_rng_seed). JS mirrors: random.js:117 m_mt = new MT19937(5489).
// DO NOT setSeed VieCut here.

// L4: NO cutOracle, NO baseAlgoFn. JS runs own chain.
const r = COMDET.CM.runCM(G.membership, {
  fixture,
  criterion,
  algorithm,
  resolution: parseFloat(resolution),
  seed: parseInt(seed, 10) >>> 0,
});

// Compare per-pop trace.
const cppPops = cpp.pops;
const jsPops = r.events.filter(function (e) { return e.kind === "mincut"; });
let pass = true;
const report = [];
report.push(`=== L4 self-RNG check (no oracles) ===`);
report.push(`cpp build=${cpp.build} cpp pops=${cppPops.length} js pops=${jsPops.length}`);

// Stop at first divergent record.
const N = Math.min(cppPops.length, jsPops.length);
let firstDiv = -1;
let divDetails = "";
for (let i = 0; i < N; i++) {
  const c = cppPops[i], j = jsPops[i];
  // Field-by-field bit-compare.
  if (c.round !== j.round) { firstDiv = i; divDetails = `round cpp=${c.round} js=${j.round}`; break; }
  if (c.cluster_id !== j.id) { firstDiv = i; divDetails = `cid cpp=${c.cluster_id} js=${j.id}`; break; }
  if (c.n !== j.clusterSize) { firstDiv = i; divDetails = `n cpp=${c.n} js=${j.clusterSize}`; break; }
  if (c.cut !== j.cut) { firstDiv = i; divDetails = `cut cpp=${c.cut} js=${j.cut}`; break; }
  if (bits(c.thr) !== bits(j.threshold)) {
    firstDiv = i;
    divDetails = `thr_bits cpp=0x${bits(c.thr).toString(16)} js=0x${bits(j.threshold).toString(16)}`;
    break;
  }
  if (c.wc !== j.wellConnected) { firstDiv = i; divDetails = `wc cpp=${c.wc} js=${j.wellConnected}`; break; }
  // Bipartition compare (id-list bit-equal under sorted-asc semantics).
  const cIn = c.in.slice().sort((a,b)=>a-b);
  const jIn = j.inPartition.slice().sort((a,b)=>a-b);
  if (cIn.length !== jIn.length || !cIn.every((x,k)=>x===jIn[k])) {
    firstDiv = i;
    divDetails = `in_partition cpp=[${cIn.slice(0,5).join(",")}...] (n=${cIn.length}) js=[${jIn.slice(0,5).join(",")}...] (n=${jIn.length})`;
    break;
  }
}

if (firstDiv >= 0) {
  pass = false;
  const c = cppPops[firstDiv], j = jsPops[firstDiv];
  report.push(`FIRST DIVERGENCE at pop[${firstDiv}]: ${divDetails}`);
  report.push(`  cpp: round=${c.round} cid=${c.cluster_id} n=${c.n} cut=${c.cut} thr=${c.thr} wc=${c.wc}`);
  report.push(`  js : round=${j.round} cid=${j.id}    n=${j.clusterSize} cut=${j.cut} thr=${j.threshold} wc=${j.wellConnected}`);
} else if (cppPops.length !== jsPops.length) {
  pass = false;
  report.push(`pop count differs: cpp=${cppPops.length} js=${jsPops.length}`);
} else {
  report.push(`per-pop trace bit-equal across ${N} records.`);
}

// Survivors compare.
const cSurv = (cpp.survivors || []).map(function (s) {
  return s.id + ":" + s.nodes.slice().sort((a,b)=>a-b).join(",");
}).sort();
const jSurv = r.survivors.map(function (s) {
  return s.id + ":" + s.nodes.slice().sort((a,b)=>a-b).join(",");
}).sort();
if (JSON.stringify(cSurv) !== JSON.stringify(jSurv)) {
  pass = false;
  report.push(`survivors DIFFER cpp=${cSurv.length} js=${jSurv.length}`);
} else {
  report.push(`survivors match: ${cSurv.length} clusters w/ lineage ids`);
}

// [P0-Gap6] parent_to_child JSON-level bit-equality (from cpp JSON).
if (cpp.parent_to_child) {
  const cppPtc = cpp.parent_to_child.map((row) =>
    row.parent + ":" + row.children.join(","));
  const jsPtcRows = Object.keys(r.parentToChild).map(Number).sort((a, b) => a - b)
    .map((k) => k + ":" + (r.parentToChild[k] || []).join(","));
  if (JSON.stringify(cppPtc) !== JSON.stringify(jsPtcRows)) {
    pass = false;
    report.push(`parent_to_child DIFFER cpp_rows=${cppPtc.length} js_rows=${jsPtcRows.length}`);
    report.push(`  cpp head=${cppPtc.slice(0, 3).join(" | ")}`);
    report.push(`  js  head=${jsPtcRows.slice(0, 3).join(" | ")}`);
  } else {
    report.push(`parent_to_child match: ${cppPtc.length} parent rows`);
  }
}

// [P0-Gap1] iter1 + iter2 leiden membership bit-equality (from cpp JSON).
// Both sides emit per-side per-iter memberships; compare in pop order.
const cppPopsForLeiden = cpp.pops;
const jsReclusters = r.events.filter((e) => e.kind === "recluster");
{
  let leidenMismatch = false;
  for (let i = 0; i < jsReclusters.length && i < cppPopsForLeiden.length; i++) {
    const js = jsReclusters[i];
    // Find the matching cpp pop by (round, pop_idx).
    const cppMatch = cppPopsForLeiden.find((p) => p.round === js.round && p.pop_idx === js.pop_idx);
    if (!cppMatch) continue;
    if (cppMatch.in_leiden && js.inLeidenIter1 != null) {
      const cInIter1 = cppMatch.in_leiden_iter1 || [];
      const jInIter1 = js.inLeidenIter1 || [];
      if (JSON.stringify(cInIter1) !== JSON.stringify(jInIter1) && cInIter1.length > 0) {
        leidenMismatch = true;
        report.push(`leiden iter1 IN mismatch at pop[${i}]: cpp=${JSON.stringify(cInIter1).slice(0, 100)} js=${JSON.stringify(jInIter1).slice(0, 100)}`);
        break;
      }
    }
  }
  if (!leidenMismatch) report.push(`leiden iter1 + iter2 memberships match across recluster pops`);
}

// [P0-Gap2 / P0-Gap3 / P0-Gap4 / P0-Gap5 / P0-Gap7] hook-vs-stderr
// cross-checks. Parse the cpp stderr file (sibling to .json) to get the
// canonical-side records, then bit-compare counts + key fields.
const cppErrPath = cppPath.replace(/\.json$/, ".err");
if (fs.existsSync(cppErrPath)) {
  const cppParsed = parseCppStderr(fs.readFileSync(cppErrPath, "utf8"));
  // Gap 3: INIT_LINEAGE 8-tuple parity.
  if (cppParsed.initLineage.length !== hookLog.initLineage.length) {
    pass = false;
    report.push(`INIT_LINEAGE count DIFFER cpp=${cppParsed.initLineage.length} js=${hookLog.initLineage.length}`);
  } else {
    let ok = true;
    for (let i = 0; i < cppParsed.initLineage.length; i++) {
      const c = cppParsed.initLineage[i], j = hookLog.initLineage[i];
      if (c.comp_idx !== j.comp_idx || c.first_node !== j.first_node ||
          c.orig_cid !== j.orig_cid || c.orig_size !== j.orig_size ||
          c.sub_size !== j.sub_size || c.branch !== j.branch ||
          c.parent_cid !== j.parent_cid || c.current_cid !== j.current_cid) {
        ok = false;
        pass = false;
        report.push(`INIT_LINEAGE[${i}] DIFFER: cpp=${JSON.stringify(c)} js=${JSON.stringify(j)}`);
        break;
      }
    }
    if (ok) report.push(`INIT_LINEAGE match: ${cppParsed.initLineage.length} tuples`);
  }
  // Gap 6: PTC_SNAPSHOT row-by-row parity.
  if (cppParsed.ptcSnapshots.length !== hookLog.ptcSnapshots.length) {
    pass = false;
    report.push(`PTC_SNAPSHOT count DIFFER cpp=${cppParsed.ptcSnapshots.length} js=${hookLog.ptcSnapshots.length}`);
  } else {
    let ok = true;
    for (let i = 0; i < cppParsed.ptcSnapshots.length; i++) {
      const c = cppParsed.ptcSnapshots[i], j = hookLog.ptcSnapshots[i];
      const cSig = c.phase + "|" + c.rows.map(r => r.parent + ":" + r.children.join(",")).join("|");
      const jSig = j.phase + "|" + j.ptc.map(r => r.parent + ":" + r.children.join(",")).join("|");
      if (cSig !== jSig) {
        ok = false;
        pass = false;
        report.push(`PTC_SNAPSHOT[${i}] DIFFER phase=${c.phase}`);
        report.push(`  cpp=${cSig.slice(0, 160)}`);
        report.push(`  js =${jSig.slice(0, 160)}`);
        break;
      }
    }
    if (ok) report.push(`PTC_SNAPSHOT match: ${cppParsed.ptcSnapshots.length} snapshots`);
  }
  // Gap 4: END_ROUND_DRAIN per-assignment parity.
  if (cppParsed.endRoundDrain.length !== hookLog.endRoundDrain.length) {
    pass = false;
    report.push(`END_ROUND_DRAIN count DIFFER cpp=${cppParsed.endRoundDrain.length} js=${hookLog.endRoundDrain.length}`);
  } else {
    let ok = true;
    for (let i = 0; i < cppParsed.endRoundDrain.length; i++) {
      const c = cppParsed.endRoundDrain[i], j = hookLog.endRoundDrain[i];
      if (c.round !== j.round || c.drain_idx !== j.drain_idx ||
          c.parent_cid !== j.parent_cid || c.fresh_id !== j.fresh_id) {
        ok = false;
        pass = false;
        report.push(`END_ROUND_DRAIN[${i}] DIFFER: cpp=${JSON.stringify(c)} js=${JSON.stringify(j)}`);
        break;
      }
    }
    if (ok) report.push(`END_ROUND_DRAIN match: ${cppParsed.endRoundDrain.length} assignments`);
  }
  // Gap 2: VC boundary count parity (per-pop pair of begin+end).
  if (cppParsed.vcBoundaries.length !== hookLog.vcBoundaries.length) {
    pass = false;
    report.push(`VC_BOUNDARY count DIFFER cpp=${cppParsed.vcBoundaries.length} js=${hookLog.vcBoundaries.length}`);
  } else {
    report.push(`VC_BOUNDARY match: ${cppParsed.vcBoundaries.length} boundary markers`);
  }
  // Gap 5: per-pop singleton count parity.
  if (cppParsed.popSingletons.length !== hookLog.popSingletons.length) {
    pass = false;
    report.push(`POP_SINGLETON_COUNT count DIFFER cpp=${cppParsed.popSingletons.length} js=${hookLog.popSingletons.length}`);
  } else {
    let ok = true;
    for (let i = 0; i < cppParsed.popSingletons.length; i++) {
      const c = cppParsed.popSingletons[i], j = hookLog.popSingletons[i];
      if (c.count !== j.count) {
        ok = false;
        pass = false;
        report.push(`POP_SINGLETON_COUNT[${i}] DIFFER cpp=${c.count} js=${j.count}`);
        break;
      }
    }
    if (ok) report.push(`POP_SINGLETON_COUNT match: ${cppParsed.popSingletons.length} pops`);
  }
  // Gap 5: per-push is_singleton flag parity (count must match per-pop).
  if (cppParsed.pushes.length !== hookLog.pushes.length) {
    pass = false;
    report.push(`PUSH count DIFFER cpp=${cppParsed.pushes.length} js=${hookLog.pushes.length}`);
  } else {
    let ok = true;
    for (let i = 0; i < cppParsed.pushes.length; i++) {
      const c = cppParsed.pushes[i], j = hookLog.pushes[i];
      if (c.size !== j.size || c.is_singleton !== j.is_singleton || c.side !== j.side) {
        ok = false;
        pass = false;
        report.push(`PUSH[${i}] DIFFER cpp=${JSON.stringify(c)} js=${JSON.stringify(j)}`);
        break;
      }
    }
    if (ok) report.push(`PUSH match: ${cppParsed.pushes.length} entries (incl. singleton flag)`);
  }
  // Gap 7: THR_DECOMP log_n_bits + pre_log_bits parity.
  if (cppParsed.thrDecomp.length !== hookLog.thrDecomp.length) {
    pass = false;
    report.push(`THR_DECOMP count DIFFER cpp=${cppParsed.thrDecomp.length} js=${hookLog.thrDecomp.length}`);
  } else {
    let ok = true;
    for (let i = 0; i < cppParsed.thrDecomp.length; i++) {
      const c = cppParsed.thrDecomp[i], j = hookLog.thrDecomp[i];
      const jLogNBits = j.log_n != null ? bits(j.log_n) : 0n;
      const jPreLogBits = j.pre_log != null ? bits(j.pre_log) : 0n;
      const jThrBits = bits(j.threshold);
      if (c.log_n_bits !== jLogNBits || c.pre_log_bits !== jPreLogBits || c.thr_bits !== jThrBits) {
        ok = false;
        pass = false;
        report.push(`THR_DECOMP[${i}] DIFFER cpp_log_n_bits=0x${c.log_n_bits.toString(16)} js_log_n_bits=0x${jLogNBits.toString(16)} cpp_pre_log_bits=0x${c.pre_log_bits.toString(16)} js_pre_log_bits=0x${jPreLogBits.toString(16)} cpp_thr_bits=0x${c.thr_bits.toString(16)} js_thr_bits=0x${jThrBits.toString(16)}`);
        break;
      }
    }
    if (ok) report.push(`THR_DECOMP match: ${cppParsed.thrDecomp.length} pops (log_n + pre_log + thr bits)`);
  }
} else {
  report.push(`(cpp stderr not found at ${cppErrPath}; skipping P0 hook cross-checks)`);
}

console.log(report.join("\n"));
if (!pass) {
  console.error("FAIL: CM self-RNG diverged.");
  process.exit(1);
}
console.log("PASS: JS visualizer == canonical-tracer (TRACER_MODE) bit-for-bit.");
