/* CC kernel cross-check, JS replay leg.
 *
 * Reads stdout JSON trace from /tmp/cc_kernel_check (the instrumented
 * C++ leg), runs the JS CC kernel on the same input, asserts that the
 * JS final-cluster decomposition is byte-equal to the C++ one.
 *
 * CC is fully deterministic (no RNG); replay is therefore trivial:
 * both legs run the same algorithm and must produce identical
 * components in identical iteration order.
 *
 * Run:
 *   /tmp/cc_kernel_check edge.csv com.csv js_out.csv > cpp.json 2>cpp.trace
 *   node tools/viz_check/cc/kernel_check.mjs cpp.json edge.csv com.csv [cpp.trace]
 *
 * When the optional cpp.trace path is supplied the replay also asserts
 * that the JS [TRACE-CC] probe stream (gated by globalThis.CC_DUMP_PROBES)
 * is byte-equal to cpp's. The CC P0 probe set covers:
 *   RIE_EDGE, RIE_SUM, CCW_ROOT, CCW_NEIGHS, CCW_VISIT, CCW_MEMBER,
 *   CCW_COMP_DONE, BUCKET, COMP_FLUSH, WCQ_POP, WriteClusterQueue.
 *
 * Exit code: 0 on byte-equal; 1 on any mismatch.
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 3) {
  console.error("usage: kernel_check.mjs <cpp_trace.json> <edge.csv> <com.csv> [cpp.trace]");
  process.exit(2);
}
const [cppTracePath, edgePath, comPath, cppTraceLogPath] = args;

const cpp = JSON.parse(fs.readFileSync(cppTracePath, "utf8"));

// Load JS kernel against the same input. Enable per-step [TRACE-CC]
// probes BEFORE importing cc.js so the module-level _ccProbesEnabled()
// gate sees the flag at first call.
globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
globalThis.CC_DUMP_PROBES = true;

// Capture JS probe emissions (stderr) and assert byte-equality vs cpp.
const jsTraceLines = [];
const origStderrWrite = process.stderr.write.bind(process.stderr);
const origConsoleError = console.error;
console.error = function (msg) {
  if (typeof msg === "string" && msg.startsWith("[TRACE-CC]")) {
    jsTraceLines.push(msg);
    return;
  }
  origConsoleError.apply(console, arguments);
};

const WEB = path.join(__dirname, "../../../../../web/vltanh.github.io/comdet/js");
await import(path.join(WEB, "mincut.js"));
await import(path.join(WEB, "louvain/louvain.js"));
await import(path.join(WEB, "leiden/leiden.js"));
await import(path.join(WEB, "comdet/page_helpers.js"));
await import(path.join(WEB, "cc/cc.js"));

function loadGraph(edgePath, comPath) {
  const edgeText = fs.readFileSync(edgePath, "utf8").trim().split(/\r?\n/);
  const com = fs.readFileSync(comPath, "utf8").trim().split(/\r?\n/);
  const orig_to_new = new Map();
  const edges = [];
  // First line is header.
  for (let i = 1; i < edgeText.length; i++) {
    if (!edgeText[i]) continue;
    const cols = edgeText[i].split(/[,\t ]/);
    const s = cols[0], t = cols[1];
    if (!orig_to_new.has(s)) orig_to_new.set(s, orig_to_new.size);
    if (!orig_to_new.has(t)) orig_to_new.set(t, orig_to_new.size);
    edges.push([orig_to_new.get(s), orig_to_new.get(t)]);
  }
  const partition = new Int32Array(orig_to_new.size);
  for (let i = 0; i < partition.length; i++) partition[i] = -2147483648;
  for (let i = 1; i < com.length; i++) {
    if (!com[i]) continue;
    const cols = com[i].split(/[,\t ]/);
    const newId = orig_to_new.get(cols[0]);
    if (newId == null) continue;
    partition[newId] = parseInt(cols[1], 10);
  }
  return {
    nodes: Array.from({ length: orig_to_new.size }, (_, i) => i),
    edges,
    membership: partition,
  };
}

const G = loadGraph(edgePath, comPath);
const fixture = { nodes: G.nodes, edges: G.edges, gt: Array.from(G.membership) };

const r = COMDET.CC.runCC(G.membership, { fixture });

// Compare component-by-component, byte-equal.
// Canonical's GetConnectedComponents filters singletons (`csize > 1`) at
// constrained.h:409 — comparison must mirror that filter on the JS side.
const cppComps = cpp.components;
const jsComps = r.allComps.filter(function (c) { return c.length > 1; });
let pass = true;
const report = [];
report.push(`cpp components: ${cppComps.length}`);
report.push(`js  components: ${jsComps.length}`);
if (cppComps.length !== jsComps.length) pass = false;
const N = Math.max(cppComps.length, jsComps.length);
for (let i = 0; i < N; i++) {
  const c = cppComps[i] || [];
  const j = jsComps[i] || [];
  const cStr = JSON.stringify(c);
  const jStr = JSON.stringify(j);
  if (cStr !== jStr) {
    pass = false;
    report.push(`  comp[${i}] DIFFER cpp=${cStr.slice(0, 60)} js=${jStr.slice(0, 60)}`);
  } else {
    report.push(`  comp[${i}] match size=${c.length}`);
  }
}

// Optional: per-step trajectory diff vs cpp's [TRACE-CC] stream.
if (cppTraceLogPath && fs.existsSync(cppTraceLogPath)) {
  const cppTrace = fs.readFileSync(cppTraceLogPath, "utf8")
                     .split(/\r?\n/)
                     .filter((l) => l.startsWith("[TRACE-CC]"));
  // Drop preamble lines that aren't per-step (PIPELINE_START / loaded /
  // ReadCommunities / GetConnectedComponents summary / DONE_QUEUE /
  // PIPELINE_END / per-comp summary rows). These reference cpp-only
  // bookkeeping (filenames, build mode, queue sizes) the JS port doesn't
  // emit. The per-step rows (RIE_EDGE, CCW_*, CCW_MEMBER, BUCKET,
  // COMP_FLUSH, WCQ_POP, RIE_SUM, WriteClusterQueue) are required to
  // match byte-for-byte.
  const STEP_TAGS = ["RIE_EDGE", "RIE_SUM", "CCW_ROOT", "CCW_NEIGHS",
                     "CCW_VISIT", "CCW_MEMBER", "CCW_COMP_DONE",
                     "BUCKET", "COMP_FLUSH", "WCQ_POP",
                     "WriteClusterQueue"];
  function isStep(line) {
    for (const tag of STEP_TAGS) {
      if (line.startsWith(`[TRACE-CC] ${tag}`)) return true;
    }
    return false;
  }
  const cppStep = cppTrace.filter(isStep);
  const jsStep = jsTraceLines.filter(isStep);
  report.push(`cpp trajectory steps: ${cppStep.length}`);
  report.push(`js  trajectory steps: ${jsStep.length}`);
  if (cppStep.length !== jsStep.length) {
    pass = false;
    report.push(`TRAJECTORY COUNT MISMATCH`);
  }
  const M = Math.min(cppStep.length, jsStep.length);
  let firstDiff = -1;
  for (let i = 0; i < M; i++) {
    if (cppStep[i] !== jsStep[i]) {
      pass = false;
      if (firstDiff < 0) {
        firstDiff = i;
        report.push(`  trajectory[${i}] DIFFER`);
        report.push(`    cpp=${cppStep[i]}`);
        report.push(`    js =${jsStep[i]}`);
      }
    }
  }
  if (firstDiff < 0 && cppStep.length === jsStep.length) {
    report.push(`trajectory: ${cppStep.length} steps byte-equal`);
  }
}

console.log = origConsoleError; // restore raw stdout for the summary
console.error = origConsoleError;
process.stdout.write(report.join("\n") + "\n");
if (!pass) {
  process.stderr.write("FAIL: CC tracer vs JS replay produced different output.\n");
  process.exit(1);
}
process.stdout.write("PASS: CC tracer == JS replay (byte-equal components + trajectory).\n");
