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
const G = COMDET.LOUVAIN.Graph(n, edges, { correctSelfLoops: false });
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
      const ok =
        jv.v === cv.v &&
        jv.fromComm === cv.fromComm &&
        jv.toComm === cv.toComm &&
        (!!jv.moved) === (!!cv.moved) &&
        jsDeltaBits === cv.dSbits &&
        (cv.dGainBits == null || jsGainBits === cv.dGainBits) &&
        cv.pass === P &&
        cv.visit === V;
      if (!ok) {
        fail++;
        if (examples.length < 10) {
          examples.push(
            `L${L}/P${P}/V${V}: js={v:${jv.v},from:${jv.fromComm},to:${jv.toComm},moved:${jv.moved},dS:${jsDeltaBits}} `
            + `cpp={pass:${cv.pass},visit:${cv.visit},v:${cv.v},from:${cv.fromComm},to:${cv.toComm},moved:${cv.moved},dS:${cv.dSbits}}`);
        }
      }
    }
  }
  if (cpIdx !== cpLv.visits.length) {
    fail++;
    examples.push(`L${L}: cpp has ${cpLv.visits.length} visits but js produced ${cpIdx}`);
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
