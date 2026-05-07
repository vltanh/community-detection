/* WCC hooks-equivalence check.
 *
 * Per byte-equal-tracer playbook step 2: JS kernel with vs without
 * harness must produce identical final state. The kernel IS the
 * tracer-without-harness; hooks (opts.trace, opts.cutOracle) are
 * conditionally applied via opts. With no opts: production path.
 * With opts.trace=true: trace strings populated but final state
 * identical.
 *
 * Run: node tools/viz_check/wcc/hooks_equiv.mjs
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = path.join(__dirname, "../../../../../web/vltanh.github.io/comdet/js");
await import(path.join(WEB, "mincut.js"));
await import(path.join(WEB, "wcc/wcc.js"));

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

const REPO = path.join(__dirname, "../../..");
const cells = [
  ["fixture32", "tests/cd_verify/fixture32_st_edge.csv", "tests/cd_verify/com_gt.csv"],
];

let pass = true;
for (const [name, eRel, cRel] of cells) {
  const G = loadGraph(path.join(REPO, eRel), path.join(REPO, cRel));
  const fixture = { nodes: G.nodes, edges: G.edges, gt: Array.from(G.membership) };
  const r1 = COMDET.WCC.runWCC(G.membership, { fixture, criterion: "1log_10(n)" });
  const r2 = COMDET.WCC.runWCC(G.membership, { fixture, criterion: "1log_10(n)", trace: true });
  let ok = r1.numClusters === r2.numClusters
        && r1.finalAssign.length === r2.finalAssign.length;
  if (ok) for (let i = 0; i < r1.finalAssign.length; i++) {
    if (r1.finalAssign[i] !== r2.finalAssign[i]) { ok = false; break; }
  }
  console.log(`${name}: ${ok ? "PASS" : "FAIL"} numClusters=${r1.numClusters} trace_lines=${r2.trace ? r2.trace.length : 0}`);
  if (!ok) pass = false;
}

if (pass) {
  console.log("PASS: WCC hooks-equivalence (trace=true vs no opts produce bit-equal finalAssign + numClusters)");
  process.exit(0);
}
console.error("FAIL: WCC hooks-equivalence");
process.exit(1);
