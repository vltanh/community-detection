/* IKC byte-equal diff harness (gold-standard tracer pipeline).
 *
 * Lockstep walks the Python canonical-tracer JSON output vs the JS kernel
 * hook-event trace, bit-compares every field, and stops at first divergence.
 *
 * Pipeline:
 *   1. spawn `instrumented/kernel_check.py` with TRACER_MODE=1 in the
 *      configured conda env; capture stdout JSON trace.
 *   2. load /home/vltanh/Documents/web/vltanh.github.io/comdet/js/ikc/ikc.js
 *      in a fresh vm context with __IKC_HOOK installed as a recorder.
 *   3. feed JS the same encounter-order nodeIds the Python side derived via
 *      pd.unique(df[[src,tgt]].values.ravel("K")) so compact-id space lines
 *      up across the two implementations.
 *   4. reshape JS event stream into the schema-shaped object (init / per-iter
 *      groups / final) and bit-compare to the Python JSON.
 *   5. exit 0 PASS, exit 1 FAIL with first-divergence dump to stderr.
 *
 * CLI:
 *   node diff.mjs <edge_csv> <k_floor> [--fixture-name <name>]
 *
 * Env:
 *   IKC_CONDA_ENV (default: nwbench)
 */
import { readFile, unlink } from "node:fs/promises";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import path from "node:path";
import vm from "node:vm";

// -- arg parse --------------------------------------------------------------

function parseArgs(argv) {
  const out = { positional: [], fixtureName: null };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--fixture-name") {
      out.fixtureName = argv[++i];
    } else if (a.startsWith("--fixture-name=")) {
      out.fixtureName = a.slice("--fixture-name=".length);
    } else {
      out.positional.push(a);
    }
  }
  return out;
}

const args = parseArgs(process.argv.slice(2));
if (args.positional.length < 2) {
  process.stderr.write(
    "usage: node diff.mjs <edge_csv> <k_floor> [--fixture-name <name>]\n",
  );
  process.exit(2);
}
const edgeCsv = path.resolve(args.positional[0]);
const kFloor = parseInt(args.positional[1], 10);
const fixtureName = args.fixtureName != null
  ? args.fixtureName
  : path.basename(edgeCsv).replace(/\.csv$/i, "");

// -- repo-relative path resolution ------------------------------------------

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
// __dirname = .../community-detection/tools/viz_check/ikc
const repoRoot = path.resolve(__dirname, "..", "..", "..");
const tracerPath = path.join(__dirname, "instrumented", "kernel_check.py");
const kernelPath = "/home/vltanh/Documents/web/vltanh.github.io/comdet/js/ikc/ikc.js";

// -- run the Python canonical tracer ----------------------------------------

function runPythonTracer(edgeCsvPath, kFloorVal, fixtureNameVal) {
  const condaEnv = process.env.IKC_CONDA_ENV || "nwbench";
  const tmpOut = `/tmp/diff_harness_${process.pid}_${Date.now()}.csv`;
  const result = spawnSync(
    "conda",
    ["run", "-n", condaEnv, "--no-capture-output",
     "python", tracerPath, edgeCsvPath, String(kFloorVal), tmpOut],
    {
      env: { ...process.env, TRACER_MODE: "1" },
      encoding: "utf8",
      maxBuffer: 1024 * 1024 * 1024,
    },
  );
  // Best-effort cleanup of the tmp CSV the tracer always writes.
  unlink(tmpOut).catch(() => {});

  if (result.status !== 0) {
    process.stderr.write("Python tracer failed (status=" + result.status + ")\n");
    if (result.stderr) process.stderr.write(result.stderr);
    process.exit(2);
  }
  const stdout = result.stdout || "";
  // The tracer writes a single JSON line; defensively trim and find the
  // first '{' so any conda banner / incidental noise is skipped.
  const start = stdout.indexOf("{");
  if (start < 0) {
    process.stderr.write("Python tracer produced no JSON on stdout.\n");
    process.stderr.write("stdout:\n" + stdout + "\n");
    process.stderr.write("stderr:\n" + (result.stderr || "") + "\n");
    process.exit(2);
  }
  const jsonText = stdout.slice(start);
  try {
    return JSON.parse(jsonText);
  } catch (e) {
    process.stderr.write("Failed to parse tracer JSON: " + e.message + "\n");
    process.stderr.write("stdout (first 2KB):\n" + stdout.slice(0, 2048) + "\n");
    process.exit(2);
  }
  // Note: fixtureNameVal is currently inferred by the tracer from the
  // basename of edgeCsvPath; passing --fixture-name through is unsupported
  // by the current kernel_check.py CLI ("python kernel_check.py <edge.csv>
  // <kfloor> <out.csv>"). If/when the tracer grows --fixture-name, plumb
  // it here. For now we override the fixture field client-side after
  // parsing, so the harness invocation remains authoritative.
}

// -- build JS-side input matching canonical encounter order -----------------

async function buildJsInput(edgeCsvPath) {
  const csv = await readFile(edgeCsvPath, "utf8");
  const lines = csv.replace(/\r\n/g, "\n").split("\n");
  // Drop trailing empties + the header row (positional read).
  const rows = [];
  for (let i = 1; i < lines.length; i++) {
    const line = lines[i];
    if (!line) continue;
    const parts = line.split(",");
    if (parts.length < 2) continue;
    rows.push([parts[0].trim(), parts[1].trim()]);
  }
  // Replicate pd.unique(df[[src,tgt]].values.ravel("K")):
  // F-order = column 0 then column 1, dedup-preserving-first-encounter.
  const seen = new Set();
  const nodeIds = [];
  for (const r of rows) { if (!seen.has(r[0])) { seen.add(r[0]); nodeIds.push(r[0]); } }
  for (const r of rows) { if (!seen.has(r[1])) { seen.add(r[1]); nodeIds.push(r[1]); } }
  return { nodeIds, edges: rows };
}

// -- run JS kernel in vm with hook recorder ---------------------------------

async function runJsKernel(nodeIds, edges, kFloorVal, fixtureNameVal) {
  const ctx = vm.createContext({ window: {}, globalThis: undefined });
  ctx.globalThis = ctx;
  ctx.window.COMDET = {};
  const trace = [];
  ctx.globalThis.__IKC_HOOK = (event, payload) => trace.push({ event, payload });
  const src = await readFile(kernelPath, "utf8");
  vm.runInContext(src, ctx);
  const IKC = ctx.window.COMDET.IKC;
  IKC.runIKC(nodeIds, edges, {
    kFloor: kFloorVal,
    canonicalGate: true,
    fixture: fixtureNameVal,
  });
  return trace;
}

// -- reshape JS event stream into the schema-shaped object ------------------

function reshapeJsTrace(events) {
  let init = null;
  let final = null;
  // iter -> { iter_start, max_k, components: [], iter_end }
  const itersById = new Map();

  function getIter(iterIdx) {
    let rec = itersById.get(iterIdx);
    if (!rec) {
      rec = { components: [] };
      itersById.set(iterIdx, rec);
    }
    return rec;
  }

  for (const ev of events) {
    if (ev.event === "init") {
      init = ev.payload;
    } else if (ev.event === "iter_start") {
      const r = getIter(ev.payload.iter);
      r.iter_start = ev.payload;
    } else if (ev.event === "max_k") {
      const r = getIter(ev.payload.iter);
      r.max_k = ev.payload;
    } else if (ev.event === "component") {
      const r = getIter(ev.payload.iter);
      r.components.push(ev.payload);
    } else if (ev.event === "bail_per_node_modularity") {
      // G8: bail-iter (d/(2L))² FP primitive payload. Lifted onto the
      // same iter record so reshape merges it with iter_start/max_k/iter_end.
      const r = getIter(ev.payload.iter);
      r.bail = ev.payload;
    } else if (ev.event === "iter_end") {
      const r = getIter(ev.payload.iter);
      r.iter_end = ev.payload;
    } else if (ev.event === "final") {
      final = ev.payload;
    }
  }

  // Order iterations by iter index ASC.
  const iterIdsAsc = Array.from(itersById.keys()).sort((a, b) => a - b);
  const iters = iterIdsAsc.map((idx) => {
    const r = itersById.get(idx);
    const start = r.iter_start || {};
    const mx = r.max_k || {};
    const end = r.iter_end || {};
    // Per-component records: strip the `iter` field that the JS hook
    // attaches but the Python schema doesn't carry inside the component
    // record (per TRACE_SCHEMA.md).
    const comps = r.components.map((p) => {
      const copy = {};
      for (const k of Object.keys(p)) {
        if (k === "iter") continue;
        copy[k] = p[k];
      }
      return copy;
    });
    const bail = r.bail || {};
    return {
      iter: idx,
      residual_n_before: start.residual_n_before,
      residual_m_before: start.residual_m_before,
      residual_compact_ids_sorted: start.residual_compact_ids_sorted,
      max_k: mx.max_k,
      core_numbers_sorted: mx.core_numbers_sorted,
      kcore_n: mx.kcore_n,
      kcore_m: mx.kcore_m,
      kcore_compact_ids_sorted: mx.kcore_compact_ids_sorted,
      components: comps,
      nodes_to_remove_sorted: end.nodes_to_remove_sorted,
      kept_clusters_this_iter: end.kept_clusters_this_iter,
      // G8: bail-iter FP primitive fields. null + [] on non-bail iters
      // (canonical-tracer emits the same default so shapes match).
      bail_L: bail.bail_L != null ? bail.bail_L : null,
      bail_per_node_modularity: bail.bail_per_node_modularity || [],
      terminated: end.terminated == null ? null : end.terminated,
    };
  });

  return {
    fixture: init ? init.fixture : null,
    k_floor: init ? init.k_floor : null,
    n_orig: init ? init.n_orig : null,
    m_orig: init ? init.m_orig : null,
    node_id_map: init ? init.node_id_map : null,
    iters: iters,
    final: final,
  };
}

// -- bit-compare primitives -------------------------------------------------

const _f64 = new Float64Array(1);
const _u64 = new BigUint64Array(_f64.buffer);

function bitsOf(x) {
  _f64[0] = x;
  return _u64[0];
}

function neqNumber(a, b) {
  if (Number.isNaN(a) && Number.isNaN(b)) return false;
  return bitsOf(a) !== bitsOf(b);
}

function repr(v) {
  if (v === null) return "null";
  if (v === undefined) return "undefined";
  if (typeof v === "number") {
    return `${v} (bits=0x${bitsOf(v).toString(16)})`;
  }
  if (typeof v === "string") return JSON.stringify(v);
  if (typeof v === "boolean") return String(v);
  if (Array.isArray(v)) {
    const head = v.slice(0, 5).map(repr).join(", ");
    return `[${head}${v.length > 5 ? `, ...(${v.length - 5} more)` : ""}] len=${v.length}`;
  }
  if (typeof v === "object") {
    const keys = Object.keys(v);
    return `{${keys.slice(0, 5).join(",")}${keys.length > 5 ? ",..." : ""}} keys=${keys.length}`;
  }
  return String(v);
}

class Divergence extends Error {
  constructor(pathStr, py, js, ctx) {
    super(`divergence at ${pathStr}`);
    this.divPath = pathStr;
    this.py = py;
    this.js = js;
    this.ctx = ctx || null;
  }
}

function diff(py, js, pathStr, ctx) {
  // Type fast-path
  if (py === js) return; // covers identical primitives + same reference
  if (py == null || js == null) {
    if (py === js) return;
    throw new Divergence(pathStr, py, js, ctx);
  }
  if (typeof py !== typeof js) {
    throw new Divergence(pathStr, py, js, ctx);
  }
  if (typeof py === "number") {
    if (typeof js !== "number") throw new Divergence(pathStr, py, js, ctx);
    if (neqNumber(py, js)) throw new Divergence(pathStr, py, js, ctx);
    return;
  }
  if (typeof py === "string" || typeof py === "boolean") {
    if (py !== js) throw new Divergence(pathStr, py, js, ctx);
    return;
  }
  if (Array.isArray(py)) {
    if (!Array.isArray(js)) throw new Divergence(pathStr, py, js, ctx);
    if (py.length !== js.length) {
      throw new Divergence(`${pathStr}.length`, py.length, js.length, ctx);
    }
    for (let i = 0; i < py.length; i++) {
      diff(py[i], js[i], `${pathStr}[${i}]`, ctx);
    }
    return;
  }
  if (typeof py === "object") {
    if (typeof js !== "object" || Array.isArray(js)) {
      throw new Divergence(pathStr, py, js, ctx);
    }
    const pk = Object.keys(py).sort();
    const jk = Object.keys(js).sort();
    if (pk.length !== jk.length || pk.some((k, i) => k !== jk[i])) {
      throw new Divergence(`${pathStr}<keys>`, pk, jk, ctx);
    }
    for (const k of pk) {
      diff(py[k], js[k], `${pathStr}.${k}`, ctx);
    }
    return;
  }
  // Fallback: anything else mismatched.
  throw new Divergence(pathStr, py, js, ctx);
}

// -- record-count proxy for trace size --------------------------------------

function countRecords(trace) {
  // top-level (1) + per-iter (1 each) + per-component (1 each) + final (1).
  let n = 2; // top + final
  for (const it of trace.iters || []) {
    n += 1;
    n += (it.components || []).length;
  }
  return n;
}

// -- main -------------------------------------------------------------------

async function main() {
  // 1. Python canonical trace.
  const pyTrace = runPythonTracer(edgeCsv, kFloor, fixtureName);
  // The harness owns the fixture name (--fixture-name overrides whatever
  // the tracer inferred from the basename). Apply the override on the
  // Python side so the contract is single-sourced from the CLI flag.
  pyTrace.fixture = fixtureName;

  // 2. JS-side input matching canonical pd.unique(F-order) encounter order.
  const { nodeIds, edges } = await buildJsInput(edgeCsv);

  // 3. JS kernel run.
  const jsEvents = await runJsKernel(nodeIds, edges, kFloor, fixtureName);

  // 4. Reshape into schema-shaped object.
  const jsTrace = reshapeJsTrace(jsEvents);

  // 5. Lockstep diff.
  try {
    diff(pyTrace, jsTrace, "$");
  } catch (e) {
    if (e instanceof Divergence) {
      const recs = countRecords(pyTrace);
      process.stderr.write(`FAIL ${fixtureName} k=${kFloor}\n`);
      process.stderr.write(`  path: ${e.divPath}\n`);
      process.stderr.write(`  py  : ${repr(e.py)}\n`);
      process.stderr.write(`  js  : ${repr(e.js)}\n`);
      // Best-effort context: walk pyTrace + jsTrace down to the parent
      // path of the divergence and dump the first 5 elements.
      try {
        const segs = e.divPath.split(/[.\[]/).filter(Boolean).map((s) => s.replace(/\]$/, ""));
        // Drop the leaf to get the parent.
        if (segs.length > 0) segs.pop();
        let p = pyTrace, j = jsTrace;
        for (const s of segs) {
          if (s === "$") continue;
          if (p != null) p = p[s];
          if (j != null) j = j[s];
        }
        process.stderr.write(`  py-parent : ${repr(p)}\n`);
        process.stderr.write(`  js-parent : ${repr(j)}\n`);
      } catch (_e) { /* best-effort only */ }
      process.stderr.write(`  records (py): ${recs}\n`);
      process.stdout.write(`FAIL ${fixtureName} k=${kFloor} at ${e.divPath}\n`);
      process.exit(1);
    }
    throw e;
  }

  // 6. PASS report.
  const recs = countRecords(pyTrace);
  const compCount = (pyTrace.iters || []).reduce(
    (n, it) => n + (it.components || []).length, 0,
  );
  process.stdout.write(
    `PASS ${fixtureName} k=${kFloor} ` +
    `iters=${(pyTrace.iters || []).length} ` +
    `components=${compCount} ` +
    `records=${recs}\n`,
  );
  process.exit(0);
}

main().catch((e) => {
  process.stderr.write("harness error: " + (e && e.stack || e) + "\n");
  process.exit(2);
});
