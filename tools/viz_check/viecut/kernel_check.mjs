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

import { readFileSync } from "node:fs";
import { loadVIECUT } from "./_loader.mjs";

const VIECUT = loadVIECUT();

function main() {
  const jsonPath = process.argv[2];
  if (!jsonPath) {
    console.error("usage: kernel_check.mjs <tracer.json>");
    process.exit(2);
  }
  const payload = JSON.parse(readFileSync(jsonPath, "utf-8"));
  let pass = 0;
  const failures = [];
  for (const cl of payload.clusters) {
    const G = new VIECUT.CactusGraph(cl.cactus, cl.n_local);
    const r = VIECUT.bipartitionFromCactusWithSweep(
      G, cl.n_local, cl.mincut, cl.bipartition);
    if (r.ok) pass++;
    else failures.push({ tag: cl.tag, n: cl.cactus.n, mincut: cl.mincut,
                         target: cl.bipartition, lastJs: r.lastJs });
  }
  console.log(`replay summary: ${pass}/${pass + failures.length} clusters PASS`);
  if (failures.length > 0) {
    for (const f of failures) {
      console.error(`  FAIL ${f.tag} cactus_n=${f.n} mincut=${f.mincut}`);
      console.error(`    target : ${f.target.join("")}`);
      console.error(`    last_js: ${(f.lastJs || []).join("")}`);
    }
    process.exit(1);
  }
}

main();
