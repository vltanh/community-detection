// Hooks-equivalence test for VieCut JS kernel.
//
// Per byte-equal-tracer skill playbook step 2: kernel with vs without
// debug hooks installed must produce IDENTICAL final state under same
// seed. Verifies the observer hook in random.js is side-effect-free.
//
// Run: node hooks_equiv.mjs <metis_file> [seed=0]

import { readFileSync, existsSync, readdirSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { loadVIECUT } from "./_loader.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, "../../..");
const FIXTURE_DIR = resolve(REPO, "tests/cd_verify");

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

function bipartitionKey(arr) {
  const a = arr.slice().sort((x, y) => x - y);
  return a.join(",");
}

function run(metisPath, seed, withObserver) {
  const { n, edges } = loadMetis(metisPath);
  const G = buildG(n, edges);
  let observed = [];
  if (withObserver) {
    VIECUT.random_functions.setObserver((rec) => observed.push(rec));
  } else {
    VIECUT.random_functions.clearObserver();
  }
  const result = VIECUT.cactus_mincut(G, { seed });
  VIECUT.random_functions.clearObserver();
  return {
    cutValue: result.cutValue,
    inKey: bipartitionKey(result.inPartition || []),
    outKey: bipartitionKey(result.outPartition || []),
    rngCount: observed.length,
  };
}

function diff(a, b) {
  const fields = ["cutValue", "inKey", "outKey"];
  for (const f of fields) {
    if (a[f] !== b[f]) {
      return `${f}: noObs=${JSON.stringify(a[f]).slice(0, 80)} obs=${JSON.stringify(b[f]).slice(0, 80)}`;
    }
  }
  return null;
}

function main() {
  const arg = process.argv[2];
  const seed = parseInt(process.argv[3] || "0", 10);
  let metisPaths;
  if (arg === "--all") {
    metisPaths = readdirSync(FIXTURE_DIR)
      .filter((f) => f.endsWith("_viecut.metis"))
      .map((f) => resolve(FIXTURE_DIR, f))
      .sort();
  } else if (arg && existsSync(arg)) {
    metisPaths = [arg];
  } else {
    console.error("usage: hooks_equiv.mjs <metis_file>|--all [seed=0]");
    process.exit(2);
  }

  let pass = 0, fail = 0;
  const failures = [];
  for (const p of metisPaths) {
    const tag = p.split("/").pop().replace(/_viecut\.metis$/, "");
    const a = run(p, seed, false);
    const b = run(p, seed, true);
    const d = diff(a, b);
    if (d === null) {
      pass++;
    } else {
      fail++;
      failures.push({ tag, d, rngCount: b.rngCount });
    }
  }
  console.log(`hooks-equiv: ${pass}/${pass + fail} PASS (seed=${seed})`);
  if (fail > 0) {
    for (const f of failures) {
      console.error(`  FAIL ${f.tag}: ${f.d} (rng_calls=${f.rngCount})`);
    }
    process.exit(1);
  }
}

main();
