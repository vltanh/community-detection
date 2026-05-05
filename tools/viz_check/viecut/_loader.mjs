// Shared module loader for the VieCut JS port under Node.
//
// The production modules under vltanh.github.io/comdet/js/viecut/ are
// browser IIFEs that self-register on `window.COMDET`. To run them in
// Node we shim `window` to globalThis, then evaluate each file in
// dependency order. Returns the resolved COMDET.VIECUT namespace.

import { existsSync, readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));

function locateJsDir() {
  const repoRel = resolve(HERE, "../../../../",
                          "vltanh.github.io/comdet/js/viecut");
  if (existsSync(repoRel)) return repoRel;
  const home = process.env.HOME || "";
  const fallback = resolve(home, "Documents/web/vltanh.github.io/comdet/js/viecut");
  if (existsSync(fallback)) return fallback;
  return null;
}

// Order matches script-tag order in wcc.html / cm.html.
const ORDER = [
  "random.js", "union_find.js", "node_bucket_pq.js", "fifo_node_bucket_pq.js",
  "mutable_graph.js", "cactus_graph.js",
  "balanced_cut_dfs.js", "most_balanced.js",
  "minimum_cut_helpers.js", "contract_graph.js", "contraction_tests.js",
  "scc.js", "noi_minimum_cut.js", "viecut_heuristic.js",
  "push_relabel.js", "heavy_edges.js", "graph_modification.js",
  "recursive_cactus.js", "cactus_mincut.js", "index.js",
];

export function loadVIECUT() {
  const dir = locateJsDir();
  if (!dir) {
    console.error("viecut JS dir not found at vltanh.github.io/comdet/js/viecut");
    process.exit(2);
  }
  globalThis.window = globalThis;
  globalThis.COMDET = globalThis.COMDET || {};
  for (const f of ORDER) {
    const code = readFileSync(resolve(dir, f), "utf-8");
    // eslint-disable-next-line no-new-func
    new Function(code).call(globalThis);
  }
  return globalThis.COMDET.VIECUT;
}
