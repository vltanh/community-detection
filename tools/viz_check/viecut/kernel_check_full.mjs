// Phase B-2 verification: JS BUILDS the cactus end-to-end and the
// resulting bipartition is compared to canonical's per cluster.
//
// Loads the comdet/js/viecut/*.js IIFEs in dependency order under a
// global `window` shim, invokes COMDET.VIECUT.cactus_mincut(G) on each
// cluster's induced subgraph, and asserts the cut value + bipartition
// match the canonical mincut binary's outputs. Mismatched bipartitions
// fall back to a start_vertex sweep on the JS-built cactus to absorb
// RNG-state divergence in the final balanced_cut_dfs invocation.
//
// Usage: node tools/viz_check/viecut/kernel_check_full.mjs <tracer.json>

import { existsSync, readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { loadVIECUT } from "./_loader.mjs";

const VIECUT = loadVIECUT();

function loadMetis(path) {
  const txt = readFileSync(path, "utf-8");
  const lines = txt.split("\n");
  const [n] = lines[0].split(/\s+/).map(Number);
  const edges = [];
  for (let i = 1; i <= n; i++) {
    if (!lines[i]) continue;
    const parts = lines[i].trim().split(/\s+/).filter((s) => s.length > 0);
    for (const t of parts) {
      const v = Number(t) - 1;
      if (i - 1 < v) edges.push([i - 1, v]);
    }
  }
  return { n, edges };
}

function buildG(n, edges) {
  const G = new VIECUT.MutableGraph();
  G.start_construction(n);
  for (const [u, v] of edges) G.new_edge(u, v, 1);
  G.finish_construction();
  return G;
}

function bipartitionMatches(target, inSet) {
  let exact = true, flip = true;
  for (let i = 0; i < target.length; i++) {
    const v = inSet.has(i) ? 1 : 0;
    if (v !== target[i]) exact = false;
    if (v !== 1 - target[i]) flip = false;
    if (!exact && !flip) return false;
  }
  return exact || flip;
}

function main() {
  const jsonPath = process.argv[2];
  if (!jsonPath) {
    console.error("usage: kernel_check_full.mjs <tracer.json>");
    process.exit(2);
  }
  const payload = JSON.parse(readFileSync(jsonPath, "utf-8"));
  const jsonDir = dirname(jsonPath);
  let pass = 0, fail = 0;
  const failures = [];
  for (const cl of payload.clusters) {
    const tag = cl.tag;
    const metisPath = resolve(jsonDir, `${tag}_viecut.metis`);
    if (!existsSync(metisPath)) continue;
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
    const target = cl.bipartition;
    const inSet = new Set(result.inPartition);
    if (bipartitionMatches(target, inSet)) {
      pass++;
      continue;
    }
    // Mismatch: cut value matches but the direct bipartition differs
    // (start_vertex RNG state diverges from canonical's). Sweep on the
    // JS-built cactus and accept if any sv reproduces canonical's pick.
    const sweep = VIECUT.bipartitionFromCactusWithSweep(
      result.cactus, result.cactus.getOriginalNodes(), result.cutValue, target);
    if (sweep.ok) pass++;
    else {
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
