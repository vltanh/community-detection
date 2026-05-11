/* Louvain L4 self-RNG bit-equal-per-step JS replay leg.
 *
 * Inputs (argv):
 *   <tracer_json>  cpp tracer stdout JSON (one stress-matrix cell)
 *   <edge_csv>     same edge list the tracer was run on
 *
 * Steps:
 *   1. Load comdet/js/louvain/louvain.js (same module the visualizer uses).
 *   2. Read edge.csv exactly the way the cpp tracer reads it (skip header,
 *      one (u,v) per line, 0-based ids, default weight 1.0).
 *   3. Build G, instantiate COMDET.LOUVAIN.run(G, Modularity, seed,
 *      {recordTrace:true}). NO oracle injection — JS uses its own MT19937,
 *      its own diffMove formula, its own iteration order.
 *   4. Walk run.levels[L].sweeps[P].traces[V] and bit-compare per-visit
 *      (v, fromComm, toComm, moved, dSbits) against the cpp tracer.
 *   5. Compare composed fine_membership + Q_final via bit reinterpret.
 *
 * Exit: 0 on PASS (every cell in the visit grid bit-equal); nonzero on
 * any mismatch with up to 10 examples logged.
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 2) {
  console.error("usage: kernel_check_l4.mjs <tracer.json> <edge.csv>");
  process.exit(2);
}
const [cppPath, edgePath] = args;
const cpp = JSON.parse(fs.readFileSync(cppPath, "utf8"));

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
// Load louvain.js from a stable path. Default: HEAD-checked-out file
// under vltanh.github.io. If LOUVAIN_JS env is set, use that instead
// (lets the driver pin a specific revision when the working tree has
// uncommitted edits to louvain.js).
const LOUVAIN_JS = process.env.LOUVAIN_JS
  || path.join(__dirname, "../../../vltanh.github.io/comdet/js/louvain/louvain.js");
if (!fs.existsSync(LOUVAIN_JS)) {
  console.error(`LOUVAIN_JS=${LOUVAIN_JS} does not exist; set env LOUVAIN_JS to a HEAD-pinned louvain.js`);
  process.exit(2);
}
// Post-2026-05-10 cross-algo isolation refactor: louvain.js consumes
// MT19937 + Graph + shuffle + range from COMDET.COMMON. Resolve common.js
// alongside louvain.js (HEAD-pinned path) or override via COMMON_JS env.
const COMMON_JS = process.env.COMMON_JS
  || path.join(path.dirname(LOUVAIN_JS), "../common/common.js");
if (fs.existsSync(COMMON_JS)) {
  await import(COMMON_JS);
}
await import(LOUVAIN_JS);

function bits(x) {
  const b = new Float64Array(1); b[0] = x;
  return new BigUint64Array(b.buffer)[0];
}
function bitsHex(x) {
  return "0x" + bits(x).toString(16).padStart(16, "0");
}

function readEdgeCsv(p) {
  const lines = fs.readFileSync(p, "utf8").split(/\r?\n/);
  const edges = [];
  let n = -1;
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i].trim();
    if (!line) continue;
    if (i === 0 && /[a-zA-Z_]/.test(line[0])) continue;  // header
    const cols = line.split(/[,\t ]+/);
    const u = parseInt(cols[0], 10);
    const v = parseInt(cols[1], 10);
    const w = cols.length > 2 ? parseFloat(cols[2]) : 1.0;
    if (Number.isNaN(u) || Number.isNaN(v)) continue;
    edges.push([u, v, w]);
    if (u > n) n = u;
    if (v > n) n = v;
  }
  return { n: n + 1, edges };
}

const { n, edges } = readEdgeCsv(edgePath);
if (n !== cpp.n) {
  console.error(`n mismatch: js=${n} cpp=${cpp.n}`);
  process.exit(1);
}

const Q = COMDET.LOUVAIN.Modularity();
// sortAdj:true mirrors externals/louvain post-convert + post-clean adj
// layout: each node's adj sorted by target-id ASC. Required for byte-
// equal vs the canonical-faithful tracer (tracer always sorts adj at
// level 0 + emits per-direction-ONCE adj at level 1+ via Graph.collapse).
// Graph moved to COMDET.COMMON in 2026-05-10 cross-algo refactor; fall
// back to legacy LOUVAIN namespace for HEAD-pinned pre-refactor JS.
const GraphFactory = (COMDET.COMMON && COMDET.COMMON.Graph) || COMDET.LOUVAIN.Graph;
const G = GraphFactory(n, edges, { correctSelfLoops: false, sortAdj: true });
const out = COMDET.LOUVAIN.run(G, Q, cpp.seed, { recordTrace: true });

// Walk levels: cpp.levels[L].visits is flat across passes; JS
// out.levels[L].sweeps[P].traces[V] is per-pass nested. Flatten JS to
// match cpp shape.
let fail = 0;
const examples = [];
let totalVisits = 0;

if (out.levels.length !== cpp.levels.length) {
  fail++;
  examples.push(`level count mismatch: js=${out.levels.length} cpp=${cpp.levels.length}`);
}

const Lmin = Math.min(out.levels.length, cpp.levels.length);
for (let L = 0; L < Lmin; L++) {
  const jsLv = out.levels[L];
  const cpLv = cpp.levels[L];
  if (jsLv.sweeps.length !== cpLv.passes) {
    fail++;
    examples.push(`L${L}: pass count mismatch js=${jsLv.sweeps.length} cpp=${cpLv.passes}`);
  }
  // Flatten JS visits across passes.
  let cpIdx = 0;
  for (let P = 0; P < jsLv.sweeps.length; P++) {
    const tr = jsLv.sweeps[P].traces;
    for (let V = 0; V < tr.length; V++) {
      const jv = tr[V];
      if (cpIdx >= cpLv.visits.length) {
        fail++;
        examples.push(`L${L}: js has more visits than cpp at L${L}/P${P}/V${V}`);
        break;
      }
      const cv = cpLv.visits[cpIdx++];
      totalVisits++;
      const jsDeltaBits = bitsHex(jv.moved ? jv.delta : 0);
      const jsGainBits  = bitsHex(jv.moved ? (jv.deltaGain != null ? jv.deltaGain : jv.delta * (cpp.m2 || 1)) : 0);
      const jsInCfromBits  = bitsHex(jv.inCfrom);
      const jsInCtoBits    = bitsHex(jv.inCto);
      const jsTotCfromBits = bitsHex(jv.totCfrom);
      const jsTotCtoBits   = bitsHex(jv.totCto);
      // P0 probe bit-projection (audit row F + J + M inputs).
      const jsKvBits          = jv.kv          != null ? bitsHex(jv.kv)          : null;
      const jsSelfLoopBits    = jv.selfLoop    != null ? bitsHex(jv.selfLoop)    : null;
      const jsDncBestBits     = jv.dncBest     != null ? bitsHex(jv.dncBest)     : null;
      const jsInCfromPreBits  = jv.inCfromPre  != null ? bitsHex(jv.inCfromPre)  : null;
      const jsTotCfromPreBits = jv.totCfromPre != null ? bitsHex(jv.totCfromPre) : null;
      const checks = [
        ["v",         jv.v === cv.v,           jv.v, cv.v],
        ["fromComm",  jv.fromComm === cv.fromComm, jv.fromComm, cv.fromComm],
        ["toComm",    jv.toComm === cv.toComm, jv.toComm, cv.toComm],
        ["moved",     (!!jv.moved) === (!!cv.moved), jv.moved, cv.moved],
        ["dSbits",    jsDeltaBits === cv.dSbits, jsDeltaBits, cv.dSbits],
        ["dGainBits", cv.dGainBits == null || jsGainBits === cv.dGainBits, jsGainBits, cv.dGainBits],
        ["pass",      cv.pass === P, P, cv.pass],
        ["visit",     cv.visit === V, V, cv.visit],
        ["inCfrom",   cv.inCfromBits == null || jsInCfromBits === cv.inCfromBits, jsInCfromBits, cv.inCfromBits],
        ["inCto",     cv.inCtoBits == null || jsInCtoBits === cv.inCtoBits, jsInCtoBits, cv.inCtoBits],
        ["totCfrom",  cv.totCfromBits == null || jsTotCfromBits === cv.totCfromBits, jsTotCfromBits, cv.totCfromBits],
        ["totCto",    cv.totCtoBits == null || jsTotCtoBits === cv.totCtoBits, jsTotCtoBits, cv.totCtoBits],
        // P0 probes (audit row F + J + M inputs). Tolerant of missing
        // cpp fields so older trace JSON still validates against the
        // legacy field set.
        ["kv",        cv.kvBits == null          || jsKvBits === cv.kvBits, jsKvBits, cv.kvBits],
        ["selfLoop",  cv.selfLoopBits == null    || jsSelfLoopBits === cv.selfLoopBits, jsSelfLoopBits, cv.selfLoopBits],
        ["dncBest",   cv.dncBestBits == null     || jsDncBestBits === cv.dncBestBits, jsDncBestBits, cv.dncBestBits],
        ["inCfromPre",  cv.inCfromPreBits == null  || jsInCfromPreBits === cv.inCfromPreBits, jsInCfromPreBits, cv.inCfromPreBits],
        ["totCfromPre", cv.totCfromPreBits == null || jsTotCfromPreBits === cv.totCfromPreBits, jsTotCfromPreBits, cv.totCfromPreBits],
      ];
      // Per-candidate (comm, dnc, gain) array comparison — P0 row J.
      // Walk both sides in neigh_pos order: vComm at slot 0 + first-seen
      // distinct nbr comms. Tie-break flips can mask via winner-matching
      // while loser-set diverges.
      if (cv.candidates != null && Array.isArray(jv.candidates)) {
        const jsCands = jv.candidates;
        const cpCands = cv.candidates;
        const sameLen = jsCands.length === cpCands.length;
        checks.push(["candLen", sameLen, jsCands.length, cpCands.length]);
        if (sameLen) {
          for (let k = 0; k < jsCands.length; k++) {
            const jc = jsCands[k];
            const cc = cpCands[k];
            const jcDncBits  = jc.dnc  != null ? bitsHex(jc.dnc)  : null;
            const jcGainBits = jc.gain != null ? bitsHex(jc.gain) : null;
            const cOk = jc.comm === cc.c;
            const dOk = cc.dncBits  == null || jcDncBits  === cc.dncBits;
            const gOk = cc.gainBits == null || jcGainBits === cc.gainBits;
            if (!cOk) checks.push([`cand${k}.comm`, false, jc.comm, cc.c]);
            if (!dOk) checks.push([`cand${k}.dnc`,  false, jcDncBits, cc.dncBits]);
            if (!gOk) checks.push([`cand${k}.gain`, false, jcGainBits, cc.gainBits]);
          }
        }
      }
      for (const [field, ok, jsVal, cpVal] of checks) {
        if (ok) continue;
        fail++;
        if (examples.length < 10) {
          examples.push(`L${L}/P${P}/V${V} [${field}]: js=${jsVal} cpp=${cpVal} (v=${jv.v} from=${jv.fromComm} to=${jv.toComm})`);
        }
        break;
      }
    }
  }
  if (cpIdx !== cpLv.visits.length) {
    fail++;
    examples.push(`L${L}: cpp has ${cpLv.visits.length} visits but js produced ${cpIdx}`);
  }
  // Per-pass quality bits.
  if (cpLv.qualityPerPassBits) {
    for (let P = 0; P < Math.min(jsLv.sweeps.length, cpLv.qualityPerPassBits.length); P++) {
      const jsQ = bitsHex(jsLv.sweeps[P].qualityAfter);
      const cpQ = cpLv.qualityPerPassBits[P];
      if (jsQ !== cpQ) {
        fail++;
        if (examples.length < 10) examples.push(`L${L}/P${P} [qualityAfter]: js=${jsQ} cpp=${cpQ}`);
      }
    }
  }
  // P0 probes — per-pass curQual + nb_moves. Audit row F + tie-break
  // site-2 (`while (nb_moves > 0 && new_qual - cur_qual > eps_impr)`).
  if (cpLv.curQualPerPassBits) {
    for (let P = 0; P < Math.min(jsLv.sweeps.length, cpLv.curQualPerPassBits.length); P++) {
      if (jsLv.sweeps[P].curQual == null) continue;
      const jsCQ = bitsHex(jsLv.sweeps[P].curQual);
      const cpCQ = cpLv.curQualPerPassBits[P];
      if (jsCQ !== cpCQ) {
        fail++;
        if (examples.length < 10) examples.push(`L${L}/P${P} [curQual]: js=${jsCQ} cpp=${cpCQ}`);
      }
    }
  }
  if (cpLv.nbMovesPerPass) {
    for (let P = 0; P < Math.min(jsLv.sweeps.length, cpLv.nbMovesPerPass.length); P++) {
      const jsNM = jsLv.sweeps[P].nbMoves;
      const cpNM = cpLv.nbMovesPerPass[P];
      if (jsNM !== cpNM) {
        fail++;
        if (examples.length < 10) examples.push(`L${L}/P${P} [nbMoves]: js=${jsNM} cpp=${cpNM}`);
      }
    }
  }
  // P0 #8 probes — per-level full in_/tot_ vectors at level entry +
  // exit. Audit rows K (snapshot inheritance) + M (accumulator update)
  // + L (encounter order). Walks every comm index 0..n_before-1 and
  // bit-compares; emits the first divergent (boundary, comm) tuple to
  // localise sub-ulp drift before P1 #9's per-c Q breakdown is needed.
  function compareBitsVec(jsBitsArr, cpHexArr, label, L) {
    if (!cpHexArr || !jsBitsArr) return;
    const lenJs = jsBitsArr.length;
    const lenCp = cpHexArr.length;
    if (lenJs !== lenCp) {
      fail++;
      if (examples.length < 10) examples.push(`L${L} [${label}.len]: js=${lenJs} cpp=${lenCp}`);
      return;
    }
    for (let ci = 0; ci < lenJs; ci++) {
      const jsHex = bitsHex(jsBitsArr[ci]);
      if (jsHex !== cpHexArr[ci]) {
        fail++;
        if (examples.length < 10) examples.push(`L${L} [${label}.c${ci}]: js=${jsHex} cpp=${cpHexArr[ci]}`);
        return;  // first divergent comm per boundary is enough.
      }
    }
  }
  compareBitsVec(jsLv.inBitsEntry,  cpLv.inBitsEntry,  "inEntry",  L);
  compareBitsVec(jsLv.totBitsEntry, cpLv.totBitsEntry, "totEntry", L);
  compareBitsVec(jsLv.inBitsExit,   cpLv.inBitsExit,   "inExit",   L);
  compareBitsVec(jsLv.totBitsExit,  cpLv.totBitsExit,  "totExit",  L);
  // Per-level total_weight pre/post + nAfterCollapse.
  if (cpLv.totalWeightBitsPre != null && jsLv.totalWeightPre != null) {
    const jsTW = bitsHex(jsLv.totalWeightPre);
    if (jsTW !== cpLv.totalWeightBitsPre) {
      fail++;
      if (examples.length < 10) examples.push(`L${L} [totalWeightPre]: js=${jsTW} cpp=${cpLv.totalWeightBitsPre}`);
    }
  }
  if (cpLv.totalWeightBitsPost != null && jsLv.totalWeightPost != null) {
    const jsTW = bitsHex(jsLv.totalWeightPost);
    if (jsTW !== cpLv.totalWeightBitsPost) {
      fail++;
      if (examples.length < 10) examples.push(`L${L} [totalWeightPost]: js=${jsTW} cpp=${cpLv.totalWeightBitsPost}`);
    }
  }
  if (cpLv.nAfterCollapse != null && jsLv.nAfterCollapse != null) {
    if (jsLv.nAfterCollapse !== cpLv.nAfterCollapse) {
      fail++;
      if (examples.length < 10) examples.push(`L${L} [nAfterCollapse]: js=${jsLv.nAfterCollapse} cpp=${cpLv.nAfterCollapse}`);
    }
  }
  // Skip post-level n2c compare: JS run does not expose pre-renumber n2c.
}

// Compare composed fine_membership.
const jsMem = Array.from(out.partition.membership());
let memMis = 0;
const Mlen = Math.min(jsMem.length, cpp.fine_membership_post_renumber.length);
for (let v = 0; v < Mlen; v++) {
  if (jsMem[v] !== cpp.fine_membership_post_renumber[v]) memMis++;
}
if (jsMem.length !== cpp.fine_membership_post_renumber.length) {
  fail++;
  examples.push(`fine_membership length: js=${jsMem.length} cpp=${cpp.fine_membership_post_renumber.length}`);
}
if (memMis > 0) {
  fail++;
  examples.push(`fine_membership: ${memMis}/${Mlen} entries differ`);
}

// Compare Q_final bits.
const jsQbits = bitsHex(out.quality);
const cpQbits = cpp.Q_final_bits;
const Qok = jsQbits === cpQbits;
if (!Qok) {
  fail++;
  examples.push(`Q_final_bits: js=${jsQbits} cpp=${cpQbits}`);
}

console.log(`L4 visits=${totalVisits} mem_mismatches=${memMis} Q_bits_eq=${Qok} fail=${fail}`);
if (fail > 0) {
  console.log("examples:");
  for (const e of examples) console.log("  " + e);
  process.exit(1);
}
console.log("PASS");
process.exit(0);
