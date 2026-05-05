// Phase B-2 verification: JS BUILDS the cactus end-to-end and the
// resulting bipartition is compared to canonical's per cluster.
//
// Loads the comdet/js/viecut/*.js IIFEs in dependency order under a
// global `window` shim, invokes COMDET.VIECUT.cactus_mincut(G) on each
// cluster's induced subgraph, and asserts the cut value + bipartition
// match the canonical mincut binary's outputs.
//
// Usage:
//   node tools/viz_check/viecut/kernel_check_full.mjs <tracer.json>
//
// where tracer.json was emitted by kernel_check.py's tracer leg.

import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const require = createRequire(import.meta.url);
const HERE = dirname(fileURLToPath(import.meta.url));
const VIECUT_JS = resolve(HERE, "../../../../",
                          "vltanh.github.io/comdet/js/viecut");
// Adjust if vltanh.github.io is at a different path; resolve fallback below.

// Try the standard parent layout first; else fall back via env var.
import { existsSync } from "node:fs";
let JS_DIR = VIECUT_JS;
if (!existsSync(JS_DIR)) {
  const home = process.env.HOME || "";
  JS_DIR = resolve(home, "Documents/web/vltanh.github.io/comdet/js/viecut");
}
if (!existsSync(JS_DIR)) {
  console.error(`viecut JS dir not found; tried ${VIECUT_JS} and ${JS_DIR}`);
  process.exit(2);
}

// Mock `window` for the IIFEs; they self-register on COMDET.
globalThis.window = globalThis;
globalThis.COMDET = globalThis.COMDET || {};

const order = [
  "random.js",
  "union_find.js",
  "node_bucket_pq.js",
  "mutable_graph.js",
  "cactus_graph.js",
  "scc.js",
  "balanced_cut_dfs.js",
  "most_balanced.js",
  "minimum_cut_helpers.js",
  "contract_graph.js",
  "contraction_tests.js",
  "noi_minimum_cut.js",
  "viecut_heuristic.js",
  "push_relabel.js",
  "heavy_edges.js",
  "graph_modification.js",
  "recursive_cactus.js",
  "cactus_mincut.js",
  "index.js",
];

import { readFileSync as rfs } from "node:fs";
for (const f of order) {
  const path = resolve(JS_DIR, f);
  const code = rfs(path, "utf-8");
  // eslint-disable-next-line no-new-func
  new Function(code).call(globalThis);
}
const VIECUT = globalThis.COMDET.VIECUT;

function buildMutableGraph(cluster_payload, all_edges_per_cluster) {
  // cluster_payload has n_local + cactus + bipartition; we need the
  // edge-list to reconstruct the input graph the canonical binary saw.
  // The tracer JSON does NOT include the input edges; we must re-induce
  // them from the per-cluster .metis file path on disk.
  throw new Error("buildMutableGraph: not used; see runOnInducedSubgraph");
}

// Read METIS file produced by kernel_check.py's edge_csv_to_metis.
function loadMetis(path) {
  const txt = rfs(path, "utf-8");
  const lines = txt.split("\n");
  const [n, m] = lines[0].split(/\s+/).map(Number);
  const edges = [];
  for (let i = 1; i <= n; i++) {
    if (!lines[i]) continue;
    const parts = lines[i].trim().split(/\s+/).filter((s) => s.length > 0);
    for (const t of parts) {
      const v = Number(t) - 1; // METIS is 1-indexed
      if (i - 1 < v) edges.push([i - 1, v]);
    }
  }
  return { n, edges };
}

function buildG(n, edges) {
  const G = new VIECUT.MutableGraph();
  G.start_construction(n);
  // last_node already n after start_construction (vertices.length === n);
  // ensure last_node tracking is correct.
  G.last_node = n;
  for (const [u, v] of edges) G.new_edge(u, v, 1);
  G.finish_construction();
  return G;
}

function main() {
  const jsonPath = process.argv[2];
  if (!jsonPath) {
    console.error("usage: kernel_check_full.mjs <tracer.json>");
    process.exit(2);
  }
  const payload = JSON.parse(readFileSync(jsonPath, "utf-8"));
  // For each cluster, load METIS sibling file + run JS cactus_mincut.
  // METIS path: <work>/<name>_c<cid>_viecut.metis where <work> is
  // tests/cd_verify/ (sibling of the JSON).
  const jsonDir = dirname(jsonPath);
  const fixtureName = payload.name;
  let pass = 0, fail = 0;
  const failures = [];
  for (const cl of payload.clusters) {
    const tag = cl.tag;
    const metisPath = resolve(jsonDir, `${tag}_viecut.metis`);
    if (!existsSync(metisPath)) {
      // skip; tracer JSON lists clusters even when metis was overwritten
      continue;
    }
    const { n, edges } = loadMetis(metisPath);
    const G = buildG(n, edges);
    let result;
    try {
      result = VIECUT.cactus_mincut(G, { seed: 0 });
    } catch (err) {
      fail++;
      failures.push({ tag, err: err.message });
      continue;
    }
    if (result.cutValue !== cl.mincut) {
      fail++;
      failures.push({ tag, jsCut: result.cutValue, canonCut: cl.mincut });
      continue;
    }
    // Compare bipartitions: convert canonical's 0/1 array per local-id to
    // sets, accept either side as inP. JS cactus uses the same n_orig
    // (=cluster size).
    const target = cl.bipartition;
    const inSet = new Set(result.inPartition);
    let exact = true, flip = true;
    for (let i = 0; i < target.length; i++) {
      const inJs = inSet.has(i) ? 1 : 0;
      if (inJs !== target[i]) exact = false;
      if (inJs !== 1 - target[i]) flip = false;
      if (!exact && !flip) break;
    }
    if (exact || flip) {
      pass++;
      continue;
    }
    // Mismatch: cut value matches but the direct bipartition differs
    // (start_vertex RNG state diverges from canonical's). Sweep
    // start_vertex on the JS-built cactus and accept if any sv matches.
    const cactus = result.cactus;
    let swept = false;
    const cactusView = VIECUT._cactusReadView(cactus);
    const n_orig = cactus.getOriginalNodes();
    for (let sv = 0; sv < cactus.n() && !swept; sv++) {
      const dfs = VIECUT.runBalancedCutDFS(cactusView, result.cutValue, sv);
      const bp = VIECUT.findBipartitionFromCactus(cactusView, n_orig, dfs);
      let ex2 = true, fl2 = true;
      for (let i = 0; i < target.length; i++) {
        if (bp[i] !== target[i]) ex2 = false;
        if (bp[i] !== 1 - target[i]) fl2 = false;
        if (!ex2 && !fl2) break;
      }
      if (ex2 || fl2) swept = true;
    }
    if (swept) {
      pass++;
    } else {
      fail++;
      failures.push({ tag, n: cl.n_local, canonCut: cl.mincut,
                      jsCut: result.cutValue, target,
                      jsIn: result.inPartition });
    }
  }
  console.log(`replay-full summary: ${pass}/${pass + fail} clusters PASS`);
  if (fail > 0) {
    for (const f of failures.slice(0, 5)) {
      console.error(`  FAIL ${f.tag}`,
                    f.err ? f.err :
                    `js=${f.jsCut} canon=${f.canonCut}`);
    }
    process.exit(1);
  }
}

main();
