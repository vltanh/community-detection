// VieCut JS replay leg (DFS-only).
//
// Loads the shipped comdet/js/viecut/*.js modules under a Node `window`
// shim and uses COMDET.VIECUT.bipartitionFromCactusWithSweep over each
// cluster's parsed cactus + bipartition to assert byte-equal selection.
//
// The cactus is structurally unique up to start-vertex choice; the JS
// replay sweeps start_vertex because canonical's choice depends on
// VieCut's std::mt19937 state at balanced_cut_dfs invocation, which we
// cannot reconstruct without re-running the full cactus_mincut path.

import { existsSync, readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
let JS_DIR = resolve(HERE, "../../../../",
                     "vltanh.github.io/comdet/js/viecut");
if (!existsSync(JS_DIR)) {
  const home = process.env.HOME || "";
  JS_DIR = resolve(home, "Documents/web/vltanh.github.io/comdet/js/viecut");
}
if (!existsSync(JS_DIR)) {
  console.error(`viecut JS dir not found; tried both repo-relative and ${JS_DIR}`);
  process.exit(2);
}

globalThis.window = globalThis;
globalThis.COMDET = globalThis.COMDET || {};

// Order matches script-tag order in wcc.html / cm.html.
const order = [
  "random.js", "union_find.js", "node_bucket_pq.js",
  "mutable_graph.js", "cactus_graph.js",
  "balanced_cut_dfs.js", "most_balanced.js",
  "minimum_cut_helpers.js", "contract_graph.js", "contraction_tests.js",
  "scc.js", "noi_minimum_cut.js", "viecut_heuristic.js",
  "push_relabel.js", "heavy_edges.js", "graph_modification.js",
  "recursive_cactus.js", "cactus_mincut.js", "index.js",
];
for (const f of order) {
  const code = readFileSync(resolve(JS_DIR, f), "utf-8");
  // eslint-disable-next-line no-new-func
  new Function(code).call(globalThis);
}
const VIECUT = globalThis.COMDET.VIECUT;

function tryCluster(cl) {
  const { tag, n_local, mincut, cactus, bipartition } = cl;
  const G = new VIECUT.CactusGraph(cactus, n_local);
  const r = VIECUT.bipartitionFromCactusWithSweep(G, n_local, mincut, bipartition);
  if (r.ok) return { ok: true, sv: r.sv, mode: r.mode };
  return { ok: false, target: bipartition, lastJs: r.lastJs };
}

function main() {
  const jsonPath = process.argv[2];
  if (!jsonPath) {
    console.error("usage: kernel_check.mjs <tracer.json>");
    process.exit(2);
  }
  const payload = JSON.parse(readFileSync(jsonPath, "utf-8"));
  let pass = 0, fail = 0;
  const failures = [];
  for (const cl of payload.clusters) {
    const r = tryCluster(cl);
    if (r.ok) pass++;
    else {
      fail++;
      failures.push({ tag: cl.tag, n: cl.cactus.n, mincut: cl.mincut,
                      target: r.target, lastJs: r.lastJs });
    }
  }
  console.log(`replay summary: ${pass}/${pass + fail} clusters PASS`);
  if (fail > 0) {
    for (const f of failures) {
      console.error(`  FAIL ${f.tag} cactus_n=${f.n} mincut=${f.mincut}`);
      console.error(`    target : ${f.target.join("")}`);
      console.error(`    last_js: ${(f.lastJs || []).join("")}`);
    }
    process.exit(1);
  }
}

main();
