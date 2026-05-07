// VieCut diff harness — bit-equal lockstep comparison of JS observer
// trace vs canonical [TRACE-RNG] stream. Per byte-equal-tracer skill
// playbook step 3.
//
// Reads canonical-tracer's stderr [TRACE-RNG] queue, runs JS port with
// observer installed under matching seed, walks both traces lockstep,
// stops at FIRST divergent record with (step_idx, field_name, cpp_val,
// js_val, Δ). uint64 reinterpret used for the (rare) double fields.
//
// Run: node diff_harness.mjs <metis_file> [seed=0]

import { readFileSync, existsSync, readdirSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { loadVIECUT } from "./_loader.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, "../../..");
const FIXTURE_DIR = resolve(REPO, "tests/cd_verify");
const TRACED = "/tmp/viecut_traced_swapped";

if (!existsSync(TRACED)) {
  console.error(`${TRACED} not built; run instrumented/build.sh`);
  process.exit(2);
}
const VIECUT = loadVIECUT();

// Bit reinterpret double -> BigInt uint64 (per playbook §3).
function bits(x) {
  const b = new Float64Array(1);
  b[0] = x;
  return new BigUint64Array(b.buffer)[0];
}

function parseRngTrace(stderr) {
  const queue = [];
  for (const line of stderr.split("\n")) {
    const m = line.match(/^\[TRACE-RNG\] kind:(\w+)(.*)$/);
    if (!m) continue;
    const kind = m[1];
    const rest = m[2];
    if (kind === "setSeed") {
      const v = rest.match(/val:(\d+)/);
      queue.push({ kind: "setSeed", seed: v ? (parseInt(v[1], 10) >>> 0) : 0 });
    } else if (kind === "next") {
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

function diffField(idx, field, a, b) {
  if (typeof a === "number" && typeof b === "number" && (!Number.isInteger(a) || !Number.isInteger(b))) {
    const ba = bits(a), bb = bits(b);
    if (ba !== bb) {
      return `step=${idx} field=${field} cpp=${a} js=${b} Δ_uint64=${(ba > bb ? ba - bb : bb - ba).toString(16)}`;
    }
    return null;
  }
  if (a !== b) return `step=${idx} field=${field} cpp=${JSON.stringify(a)} js=${JSON.stringify(b)}`;
  return null;
}

function compareRecord(idx, cpp, js) {
  if (cpp.kind !== js.kind) return `step=${idx} kind: cpp=${cpp.kind} js=${js.kind}`;
  if (cpp.kind === "setSeed") return diffField(idx, "seed", cpp.seed, js.seed);
  if (cpp.kind === "next") return diffField(idx, "val", cpp.val, js.val);
  if (cpp.kind === "nextInt") {
    return diffField(idx, "lb", cpp.lb, js.lb)
        || diffField(idx, "rb", cpp.rb, js.rb)
        || diffField(idx, "val", cpp.val, js.val);
  }
  if (cpp.kind === "nextBool") return diffField(idx, "val", cpp.val, js.val);
  if (cpp.kind === "nextDouble") return diffField(idx, "val", cpp.val, js.val);
  return null;
}

function runOne(metisPath, seed) {
  const tag = metisPath.split("/").pop().replace(/_viecut\.metis$/, "");
  const rc = spawnSync(TRACED, [metisPath, String(seed)], { encoding: "utf-8", maxBuffer: 1 << 28 });
  if (rc.status !== 0) return { tag, ok: false, err: `tracer exit ${rc.status}` };
  if (!rc.stdout.trim().startsWith("{")) return { tag, ok: true, skipped: true };
  const canon = JSON.parse(rc.stdout);
  if (canon.mincut <= 0) return { tag, ok: true, skipped: true };

  const cppQueue = parseRngTrace(rc.stderr);

  const { n, edges } = loadMetis(metisPath);
  const G = buildG(n, edges);
  const jsQueue = [];
  VIECUT.random_functions.setObserver((rec) => jsQueue.push(rec));
  try {
    VIECUT.cactus_mincut(G, { seed });
  } catch (err) {
    VIECUT.random_functions.clearObserver();
    return { tag, ok: false, err: err.message };
  }
  VIECUT.random_functions.clearObserver();

  // setSeed records: cpp tracer emits setSeed; JS also emits via observer
  // when cactus_mincut calls setSeed. Both should produce 1 setSeed entry.
  // Walk lockstep.
  const N = Math.min(cppQueue.length, jsQueue.length);
  for (let i = 0; i < N; i++) {
    const d = compareRecord(i, cppQueue[i], jsQueue[i]);
    if (d !== null) {
      const ctx = [];
      for (let j = Math.max(0, i - 2); j < Math.min(N, i + 3); j++) {
        ctx.push(`  [${j}] cpp=${JSON.stringify(cppQueue[j]).slice(0, 80)} js=${JSON.stringify(jsQueue[j]).slice(0, 80)}`);
      }
      return { tag, ok: false, err: d, ctx, cpp_n: cppQueue.length, js_n: jsQueue.length };
    }
  }
  if (cppQueue.length !== jsQueue.length) {
    return { tag, ok: false,
             err: `record count mismatch cpp=${cppQueue.length} js=${jsQueue.length}` };
  }
  return { tag, ok: true, n: cppQueue.length };
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
    console.error("usage: diff_harness.mjs <metis_file>|--all [seed=0]");
    process.exit(2);
  }

  let pass = 0, fail = 0, skip = 0, total = 0;
  const failures = [];
  for (const p of metisPaths) {
    const r = runOne(p, seed);
    if (r.skipped) skip++;
    else if (r.ok) { pass++; total += r.n; }
    else { fail++; failures.push(r); }
  }
  console.log(`diff-harness: ${pass}/${pass + fail} PASS, ${skip} skipped (seed=${seed}, total_records=${total})`);
  if (fail > 0) {
    for (const f of failures) {
      console.error(`  FAIL ${f.tag}: ${f.err}`);
      if (f.ctx) f.ctx.forEach((l) => console.error(l));
    }
    process.exit(1);
  }
}

main();
