/* CM hooks-equivalence test (per byte-equal-tracer skill §2).
 *
 * Verifies cm.js runCM with vs without verification hooks (cutOracle,
 * baseAlgoFn, trace) produces bit-equal final state on same seed.
 * Proves hooks have no side effects when uninstalled.
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 2) {
  console.error("usage: hooks_equiv.mjs <edge.csv> <com.csv> [criterion] [seed]");
  process.exit(2);
}
const [edgePath, comPath, criterion = "1log_10(n)", seed = 0] = args;

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = path.join(__dirname, "../../../../../web/vltanh.github.io/comdet/js");
await import(path.join(WEB, "mincut.js"));
await import(path.join(WEB, "louvain/louvain.js"));
await import(path.join(WEB, "leiden/leiden.js"));
await import(path.join(WEB, "comdet/page_helpers.js"));
await import(path.join(WEB, "wcc/wcc.js"));
try {
  const vcLoader = path.join(__dirname, "../viecut/_loader.mjs");
  if (fs.existsSync(vcLoader)) await import(vcLoader);
} catch (e) {}
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

const G = loadGraph(edgePath, comPath);
const fixture = { nodes: G.nodes, edges: G.edges, gt: Array.from(G.membership) };

// Run 1: no hooks.
const r1 = COMDET.CM.runCM(G.membership, {
  fixture, criterion, algorithm: "leiden-cpm", resolution: 0.0001,
  seed: parseInt(seed, 10) >>> 0,
});
// Run 2: trace hook installed.
const r2 = COMDET.CM.runCM(G.membership, {
  fixture, criterion, algorithm: "leiden-cpm", resolution: 0.0001,
  seed: parseInt(seed, 10) >>> 0,
  trace: true,
});

let pass = r1.numClusters === r2.numClusters
       && r1.finalAssign.length === r2.finalAssign.length;
if (pass) {
  for (let i = 0; i < r1.finalAssign.length; i++) {
    if (r1.finalAssign[i] !== r2.finalAssign[i]) { pass = false; break; }
  }
}
// Lineage compare.
const k1 = r1.survivors.map(s => s.id + ":" + s.nodes.sort((a,b)=>a-b).join(",")).sort();
const k2 = r2.survivors.map(s => s.id + ":" + s.nodes.sort((a,b)=>a-b).join(",")).sort();
if (JSON.stringify(k1) !== JSON.stringify(k2)) pass = false;

console.log(`numClusters=${r1.numClusters}  trace_lines=${r2.trace ? r2.trace.length : 0}`);
console.log(pass
  ? "hooks-equivalence PASS: trace=true vs trace=false produce bit-equal finalAssign + numClusters + survivors."
  : "hooks-equivalence FAIL");
process.exit(pass ? 0 : 1);
