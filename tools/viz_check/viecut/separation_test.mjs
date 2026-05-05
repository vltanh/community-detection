// Random/deterministic separation test.
//
// For each cluster:
// 1. Run instrumented C++ /tmp/viecut_traced. Capture [TRACE-RNG] stderr
//    sequence + cactus stdout JSON.
// 2. Parse [TRACE-RNG] into an oracle queue (list of {kind, val, ...}).
// 3. Run JS COMDET.VIECUT.cactus_mincut(G) with rngOracle attached;
//    JS consumes RNG values from the canonical C++ stream instead of
//    advancing its own MT19937.
// 4. Compare JS output (cactus tree internal-adj-order + bipartition +
//    cut value) byte-for-byte to canonical's stdout JSON.
//
// If JS produces byte-equal output WHEN FED CANONICAL'S RNG, the JS
// deterministic functions byte-match canonical's deterministic functions
// (independent of RNG implementation correctness).
//
// Usage: node separation_test.mjs <metis_file>
//        node separation_test.mjs --all   (runs all *_viecut.metis under tests/cd_verify/)

import { existsSync, readdirSync, readFileSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { loadVIECUT } from "./_loader.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, "../../..");
const TRACED = "/tmp/viecut_traced";
const FIXTURE_DIR = resolve(REPO, "tests/cd_verify");

if (!existsSync(TRACED)) {
  console.error(`/tmp/viecut_traced not built; run instrumented/build.sh first`);
  process.exit(2);
}

const VIECUT = loadVIECUT();

function parseRngTrace(stderr) {
  const queue = [];
  for (const line of stderr.split("\n")) {
    const m = line.match(/^\[TRACE-RNG\] kind:(\w+)(.*)$/);
    if (!m) continue;
    const kind = m[1];
    const rest = m[2];
    if (kind === "setSeed") continue;  // JS calls setSeed itself
    if (kind === "next") {
      const v = rest.match(/val:0x([0-9a-fA-F]+)/);
      queue.push({ kind: "next", val: parseInt(v[1], 16) >>> 0 });
    } else if (kind === "nextInt") {
      const lb = parseInt(rest.match(/lb:(\d+)/)[1], 10);
      const rb = parseInt(rest.match(/rb:(\d+)/)[1], 10);
      const val = parseInt(rest.match(/val:(\d+)/)[1], 10);
      queue.push({ kind: "nextInt", lb, rb, val });
    } else if (kind === "nextBool") {
      const val = parseInt(rest.match(/val:(\d+)/)[1], 10) === 1;
      queue.push({ kind: "nextBool", val });
    } else if (kind === "nextDouble") {
      const val = parseFloat(rest.match(/val:([-\d.eE+]+)/)[1]);
      queue.push({ kind: "nextDouble", val });
    }
  }
  return queue;
}

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

function compareCactus(jsCactus, canon) {
  if (jsCactus.n() !== canon.n) return `n mismatch: js=${jsCactus.n()} canon=${canon.n}`;
  for (let n = 0; n < canon.n; n++) {
    const jsCv = jsCactus.containedVertices(n);
    const cCv = canon.nodes[n].contained;
    if (jsCv.length !== cCv.length) return `node ${n} contained length`;
    for (let i = 0; i < jsCv.length; i++) {
      if (jsCv[i] !== cCv[i]) return `node ${n} contained[${i}] js=${jsCv[i]} canon=${cCv[i]}`;
    }
    const ne = jsCactus.get_first_invalid_edge(n);
    if (ne !== canon.adj[n].length) return `node ${n} adj length js=${ne} canon=${canon.adj[n].length}`;
    for (let e = 0; e < ne; e++) {
      const jt = jsCactus.getEdgeTarget(n, e);
      const jw = jsCactus.getEdgeWeight(n, e);
      const ct = canon.adj[n][e].target;
      const cw = canon.adj[n][e].weight;
      if (jt !== ct || jw !== cw) {
        return `node ${n} adj[${e}] js=(${jt},${jw}) canon=(${ct},${cw})`;
      }
    }
  }
  return null;
}

function runOne(metisPath) {
  const tag = metisPath.split("/").pop().replace(/_viecut\.metis$/, "");
  const rc = spawnSync(TRACED, [metisPath, "0"], { encoding: "utf-8", maxBuffer: 1 << 28 });
  if (rc.status !== 0) {
    return { tag, ok: false, err: `tracer exit ${rc.status}: ${rc.stderr.slice(-200)}` };
  }
  if (!rc.stdout.trim().startsWith("{")) {
    // mincut=0 disconnected; tracer prints log line instead of JSON.
    return { tag, ok: true, skipped: true };
  }
  const canon = JSON.parse(rc.stdout);
  if (canon.mincut <= 0) return { tag, ok: true, skipped: true };
  const queue = parseRngTrace(rc.stderr);

  const { n, edges } = loadMetis(metisPath);
  const G = buildG(n, edges);

  // Hand JS the canonical RNG stream; clear after run.
  VIECUT.random_functions.setSeed(0);
  VIECUT.random_functions.setOracle(queue);
  let result;
  try {
    result = VIECUT.cactus_mincut(G, { seed: 0 });
  } catch (err) {
    VIECUT.random_functions.clearOracle();
    return { tag, ok: false, err: err.message, queue_left: queue.length };
  }
  VIECUT.random_functions.clearOracle();

  if (result.cutValue !== canon.mincut) {
    return { tag, ok: false, err: `cut: js=${result.cutValue} canon=${canon.mincut}`,
             queue_left: queue.length };
  }
  const cactusErr = compareCactus(result.cactus, canon.cactus);
  if (cactusErr) {
    return { tag, ok: false, err: `cactus: ${cactusErr}`, queue_left: queue.length };
  }
  // Bipartition: both sides accept (canon-internal flag stored as 0/1).
  const inSet = new Set(result.inPartition);
  let exact = true, flip = true;
  for (let i = 0; i < canon.bipartition.length; i++) {
    const v = inSet.has(i) ? 1 : 0;
    if (v !== canon.bipartition[i]) exact = false;
    if (v !== 1 - canon.bipartition[i]) flip = false;
  }
  if (!exact && !flip) {
    return { tag, ok: false, err: "bipartition mismatch", queue_left: queue.length };
  }
  return { tag, ok: true, queue_left: queue.length, mode: exact ? "exact" : "flipped" };
}

function main() {
  const arg = process.argv[2];
  let metisPaths;
  if (arg === "--all") {
    metisPaths = readdirSync(FIXTURE_DIR)
      .filter((f) => f.endsWith("_viecut.metis"))
      .map((f) => resolve(FIXTURE_DIR, f))
      .sort();
  } else if (arg) {
    metisPaths = [arg];
  } else {
    console.error("usage: separation_test.mjs <metis_file> | --all");
    process.exit(2);
  }

  let pass = 0, fail = 0, skip = 0;
  const failures = [];
  for (const p of metisPaths) {
    const r = runOne(p);
    if (r.skipped) skip++;
    else if (r.ok) pass++;
    else { fail++; failures.push(r); }
  }
  console.log(`separation: ${pass}/${pass + fail} PASS, ${skip} skipped (mincut=0)`);
  if (fail > 0) {
    for (const f of failures) {
      console.error(`  FAIL ${f.tag}: ${f.err} (queue_left=${f.queue_left})`);
    }
    process.exit(1);
  }
}

main();
