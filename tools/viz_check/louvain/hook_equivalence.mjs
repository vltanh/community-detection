/* Louvain JS kernel hook-installed-vs-uninstalled equivalence test.
 *
 * Per byte-equal-tracer skill §"Step 2 — Build JS kernel = production
 * walker + conditional debug hooks": the kernel IS the tracer when no
 * harness is installed; with hooks installed it must produce IDENTICAL
 * final state on the same seed. This script verifies that.
 *
 * Hook surface for Louvain JS:
 *   - opts.recordTrace = true: collect per-visit traces in sweep().
 *     Pure side-effect-free observer; zero-cost when omitted.
 *   - opts.visitOrder: opt-in pre-shuffled visit order (used by the L3
 *     oracle-replay path; not exercised by self-RNG L4).
 *
 * The "kernel without hooks" run uses {} opts. The "kernel with hooks"
 * run uses {recordTrace: true}. Both must produce identical final
 * membership + Q_final bits.
 *
 * Run:
 *     node tools/viz_check/louvain/hook_equivalence.mjs <edge.csv> <seed>
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 2) {
  console.error("usage: hook_equivalence.mjs <edge.csv> <seed>");
  process.exit(2);
}
const [edgePath, seedArg] = args;
const seed = parseInt(seedArg, 10);

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const LOUVAIN_JS = process.env.LOUVAIN_JS
  || path.join(__dirname, "../../../vltanh.github.io/comdet/js/louvain/louvain.js");
// Cross-algo isolation refactor (2026-05-10): louvain.js consumes
// MT19937/Graph/shuffle/range from COMDET.COMMON. Resolve alongside.
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

function readEdgeCsv(p) {
  const lines = fs.readFileSync(p, "utf8").split(/\r?\n/);
  const edges = [];
  let n = -1;
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i].trim();
    if (!line) continue;
    if (i === 0 && /[a-zA-Z_]/.test(line[0])) continue;
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

function runLouvain(opts) {
  const Q = COMDET.LOUVAIN.Modularity();
  const GraphFactory = (COMDET.COMMON && COMDET.COMMON.Graph) || COMDET.LOUVAIN.Graph;
  const G = GraphFactory(n, edges, { correctSelfLoops: false, sortAdj: true });
  return COMDET.LOUVAIN.run(G, Q, seed, opts);
}

const noHooks = runLouvain({});
const withHooks = runLouvain({ recordTrace: true });

const memA = Array.from(noHooks.partition.membership());
const memB = Array.from(withHooks.partition.membership());
let memEq = memA.length === memB.length;
let memDiffs = 0;
for (let v = 0; v < Math.min(memA.length, memB.length); v++) {
  if (memA[v] !== memB[v]) { memDiffs++; memEq = false; }
}
const qA = bits(noHooks.quality);
const qB = bits(withHooks.quality);
const qEq = qA === qB;

console.log(`hooks-off vs hooks-on: mem_eq=${memEq} mem_diffs=${memDiffs} Q_bits_eq=${qEq}`);
if (!memEq || !qEq) {
  console.log(`  noHooks Q=${qA.toString(16)}`);
  console.log(`  withHooks Q=${qB.toString(16)}`);
  process.exit(1);
}
console.log("PASS");
process.exit(0);
