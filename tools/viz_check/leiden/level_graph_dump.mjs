/* Per-level LEVEL_GRAPH dump on the JS side, matching the cpp tracer's
 * [TRACE-LD-LG] hex format. Used to byte-equal-diff per-level graph +
 * partition admin state at every collapse boundary.
 *
 *   node level_graph_dump.mjs <edge.csv> <renum.json> <quality:cpm|mod> <param> <seed>
 *
 * <renum.json> is the cpp tracer's emitted JSON whose `renum_to_orig`
 * gives the canonical node-id ordering (so JS sees the same internal
 * indexing as cpp).
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 5) {
  console.error("usage: level_graph_dump.mjs <edge.csv> <cpp.json> <q> <param> <seed>");
  process.exit(2);
}
const [edgePath, cppPath, quality, paramStr, seedStr] = args;
const param = parseFloat(paramStr);
const seed = parseInt(seedStr, 10);

const cpp = JSON.parse(fs.readFileSync(cppPath, "utf8"));

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = path.join(__dirname, "../../../../../web/vltanh.github.io/comdet/js");
await import(path.join(WEB, "louvain/louvain.js"));
await import(path.join(WEB, "leiden/leiden.js"));

function loadEdges(edgePath, renum) {
  const text = fs.readFileSync(edgePath, "utf8").trim().split(/\r?\n/);
  const orig_to_new = new Map();
  renum.forEach(function (orig, newId) { orig_to_new.set(orig, newId); });
  const edges = [];
  for (let i = 1; i < text.length; i++) {
    if (!text[i]) continue;
    const cols = text[i].split(/[,\t ]/);
    const u = orig_to_new.get(cols[0]);
    const v = orig_to_new.get(cols[1]);
    if (u == null || v == null) continue;
    edges.push([u, v]);
  }
  return edges;
}

const renum = cpp.renum_to_orig;
const n = renum.length;
const edges = loadEdges(edgePath, renum);
const G = COMDET.LOUVAIN.Graph(n, edges, { correctSelfLoops: false, sortAdj: true });

function canonCPM(resolution) {
  return {
    name: "canonCPM", resolution,
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
        const w = P.totalWeightInComm(c);
        const possible = csl ? (nc * nc) : (nc * (nc - 1));
        mod += w - resolution * possible / 2;
      }
      return 2 * mod;
    },
  };
}
function canonMod() {
  return {
    name: "canonMod", resolution: 1.0,
    diffMove(P, v, newComm) {
      const oldComm = P.memberOf(v);
      if (oldComm === newComm) return 0;
      const Gp = P.graph;
      const m_orig = Gp.totalEdgeWeight();
      if (m_orig === 0) return 0;
      const directed = Gp.isDirected();
      const total_weight = m_orig * (directed ? 1.0 : 2.0);
      const w_to_old = P.weightToComm(v, oldComm);
      const w_from_old = P.weightFromComm(v, oldComm);
      const w_to_new = P.weightToComm(v, newComm);
      const w_from_new = P.weightFromComm(v, newComm);
      const k_out = Gp.strengthLeiden(v);
      const k_in = directed ? Gp.strengthLeiden(v) : k_out;
      const sw = Gp.nodeSelfWeight(v);
      const K_out_old = P.totalWeightFromComm(oldComm);
      const K_in_old  = P.totalWeightToComm(oldComm);
      const K_out_new = P.totalWeightFromComm(newComm) + k_out;
      const K_in_new  = P.totalWeightToComm(newComm) + k_in;
      const diff_old = (w_to_old - k_out * K_in_old / total_weight)
                     + (w_from_old - k_in * K_out_old / total_weight);
      const diff_new = (w_to_new + sw - k_out * K_in_new / total_weight)
                     + (w_from_new + sw - k_in * K_out_new / total_weight);
      const diff = diff_new - diff_old;
      const m = directed ? m_orig : 2.0 * m_orig;
      return diff / m;
    },
    quality(P) {
      const Gp = P.graph;
      const m_orig = Gp.totalEdgeWeight();
      if (m_orig === 0) return 0;
      const directed = Gp.isDirected();
      const m = directed ? m_orig : 2.0 * m_orig;
      let mod = 0;
      for (let c = 0; c < P.ncomm(); c++) {
        const w = P.totalWeightInComm(c);
        const w_out = P.totalWeightFromComm(c);
        const w_in = P.totalWeightToComm(c);
        mod += w - w_out * w_in / ((directed ? 1.0 : 4.0) * m_orig);
      }
      const q = (directed ? 1.0 : 2.0) * mod;
      return q / m;
    },
  };
}

let qfn;
if (quality === "cpm") qfn = canonCPM(param);
else if (quality === "mod") qfn = canonMod();
else { console.error("unknown quality"); process.exit(2); }

const _bbuf = new Float64Array(1);
const _bview = new BigUint64Array(_bbuf.buffer);
function hex(x) {
  _bbuf[0] = x;
  return _bview[0].toString(16).padStart(16, "0");
}

function dumpLevel(idx, g, p) {
  const K = g.vcount(), E = g.ecount();
  const directed = g.isDirected() ? 1 : 0;
  const csl = g.correctSelfLoops() ? 1 : 0;
  const tw = g.totalEdgeWeight();
  console.error(`[TRACE-LD-LG] LEVEL_GRAPH level=${idx} K=${K} E=${E} directed=${directed} csl=${csl} tw=${hex(tw)}`);
  for (let v = 0; v < K; v++) {
    const ns = g.nodeSize(v);
    const sw = g.nodeSelfWeight(v);
    // cpp's strength(v, IGRAPH_OUT) under undirected default IGRAPH_LOOPS_TWICE
    // = sum incident edge weights + nbSelfLoops self-loop counted twice
    //   (once via edge listing for u-side, once for v-side under IGRAPH_OUT
    //   for undirected graphs igraph counts self-loops twice).
    // Mirrors strengthLeiden = wDeg + nbSelfLoops.
    const sout = g.strengthLeiden(v);
    const sin = g.strengthLeiden(v);
    console.error(`[TRACE-LD-LG] VERT level=${idx} v=${v} nsize=${hex(ns)} nsw=${hex(sw)} sout=${hex(sout)} sin=${hex(sin)}`);
  }
  for (let e = 0; e < E; e++) {
    const ed = g.edge(e);
    const w = g.edgeWeight(e);
    console.error(`[TRACE-LD-LG] EDGE level=${idx} e=${e} from=${ed[0]} to=${ed[1]} w=${hex(w)}`);
  }
  const nc = p.ncomm();
  for (let c = 0; c < nc; c++) {
    const cs = p.csize(c);
    const cn = p.cnodes(c);
    const tin = p.totalWeightInComm(c);
    const tfrom = p.totalWeightFromComm(c);
    const tto = p.totalWeightToComm(c);
    console.error(`[TRACE-LD-LG] COMM level=${idx} c=${c} cnodes=${cn} csize=${hex(cs)} tin=${hex(tin)} tfrom=${hex(tfrom)} tto=${hex(tto)}`);
  }
}

const out = COMDET.LEIDEN.optimisePartition(G, qfn, seed >>> 0, {
  recordTrace: false,
  onLevelEntry: dumpLevel,
});
console.error(`# done: levels=${out.levels.length} Q_final=${out.quality}`);
