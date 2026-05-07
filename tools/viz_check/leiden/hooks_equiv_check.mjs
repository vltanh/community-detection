/* JS kernel hooks-installed-vs-uninstalled equivalence test.
 *
 * Per byte-equal-tracer skill step 2: kernel must produce IDENTICAL
 * final state with vs without recordTrace hooks. recordTrace + record-
 * Candidates flags are the hooks; without them, kernel is the visualizer
 * page import path.
 *
 * Loads dnc fixture, runs optimisePartition under matching seed in both
 * modes, bit-compares Q_final + membership.
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = path.join(__dirname, "../../../../../web/vltanh.github.io/comdet/js");
await import(path.join(WEB, "louvain/louvain.js"));
await import(path.join(WEB, "leiden/leiden.js"));

function loadEdgeIds(edgePath) {
  const text = fs.readFileSync(edgePath, "utf8").trim().split(/\r?\n/);
  const ids = new Map();
  let nextId = 0;
  const edges = [];
  for (let i = 1; i < text.length; i++) {
    if (!text[i]) continue;
    const cols = text[i].split(/[,\t ]/);
    if (cols.length < 2) continue;
    let u = ids.get(cols[0]);
    if (u == null) { u = nextId++; ids.set(cols[0], u); }
    let v = ids.get(cols[1]);
    if (v == null) { v = nextId++; ids.set(cols[1], v); }
    edges.push([u, v]);
  }
  return { n: nextId, edges };
}

const FIX = path.join(__dirname, "../../../tests/cd_verify/dnc_edge.csv");
const { n, edges } = loadEdgeIds(FIX);
const SEED = 42;

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
        const w = P.totalWeightInComm(c) / 2;
        const possible = csl ? (nc * nc) : (nc * (nc - 1));
        mod += w - resolution * possible / 2;
      }
      return 2 * mod;
    },
  };
}

const _bbuf = new Float64Array(1);
const _bview = new BigUint64Array(_bbuf.buffer);
function bits(x) { _bbuf[0] = x; return _bview[0]; }

function run(opts) {
  const G = COMDET.LOUVAIN.Graph(n, edges, { correctSelfLoops: false, sortAdj: true });
  const out = COMDET.LEIDEN.optimisePartition(G, canonCPM(0.01), SEED, opts);
  const mem = Array.from(out.partition.membership());
  return { Q: out.quality, mem };
}

const noHooks = run({});
const withHooks = run({ recordTrace: true, recordCandidates: true, maxOuterLevels: 1 });
const withMaxOuter = run({ maxOuterLevels: 1 });
const withRecordOnly = run({ recordTrace: true, maxOuterLevels: 1 });

function bitsEq(a, b) {
  return bits(a) === bits(b);
}
function memEq(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

const cmp1 = bitsEq(withMaxOuter.Q, withRecordOnly.Q) && memEq(withMaxOuter.mem, withRecordOnly.mem);
const cmp2 = bitsEq(withMaxOuter.Q, withHooks.Q) && memEq(withMaxOuter.mem, withHooks.mem);

console.log("=== Hooks-installed-vs-uninstalled equivalence ===");
console.log(`fixture: dnc (n=${n}), seed=${SEED}, qfn=canonCPM(0.01)`);
console.log(`no hooks (full multi-level):           Q=${noHooks.Q}`);
console.log(`maxOuterLevels=1 only:                 Q=${withMaxOuter.Q}`);
console.log(`maxOuterLevels=1 + recordTrace:        Q=${withRecordOnly.Q}`);
console.log(`maxOuterLevels=1 + recordTrace + cand: Q=${withHooks.Q}`);
console.log(`recordTrace flag is side-effect-free:  ${cmp1 ? "PASS" : "FAIL"}`);
console.log(`recordCandidates flag is side-effect-free: ${cmp2 ? "PASS" : "FAIL"}`);
if (!cmp1 || !cmp2) process.exit(1);
console.log("PASS: hooks have no side effects when uninstalled.");
