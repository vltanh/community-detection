/* Infomap JS kernel hooks-installed-vs-uninstalled equivalence test.
 *
 * Per byte-equal-tracer skill §"Hooks-installed-vs-uninstalled":
 * the kernel must produce IDENTICAL final state with vs without the
 * P0 probe hooks installed (__INFOMAP_DL_TERMS, __INFOMAP_UPDATE_TERMS,
 * __INFOMAP_CONSOL_BEGIN/PRE/OP/SORTED). When the hooks are absent the
 * `typeof === "function"` guards short-circuit; with the hooks installed
 * we still must observe bit-equal finalL + bit-equal membership.
 *
 * Loads the small 32-node fixture (fixture32) under matching seed and
 * runs runInfomapCanonical twice: once with no hooks, once with all
 * P0 hooks attached as no-op functions (calls accepted, discarded).
 * Bit-compares finalL + membership across the two runs.
 */
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
const __dirname = path.dirname(fileURLToPath(import.meta.url));

globalThis.window = globalThis;
globalThis.window.COMDET = { FIXTURE: { nodes: [], edges: [], gt: [] } };
const WEB = path.join(__dirname, "../../../../../web/vltanh.github.io/comdet/js");
await import(path.join(WEB, "common/common.js"));
await import(path.join(WEB, "louvain/louvain.js"));
await import(path.join(WEB, "infomap/infomap_canon.js"));

function loadEdgeIds(edgePath) {
  // Mirror canonical_run.py: pd.unique over ravel('K') over both
  // columns ordered srcs-then-tgts; intern srcs in row order, then
  // tgts in row order. cpp tracer's main_traced.cpp does the same.
  const text = fs.readFileSync(edgePath, "utf8").trim().split(/\r?\n/);
  const srcs = [], tgts = [];
  for (let i = 1; i < text.length; i++) {
    if (!text[i]) continue;
    const cols = text[i].split(/[,\t ]/);
    if (cols.length < 2) continue;
    const u = parseInt(cols[0], 10);
    const v = parseInt(cols[1], 10);
    if (u === v) continue;
    srcs.push(u); tgts.push(v);
  }
  const nodeIds = [];
  const id2idx = new Map();
  for (const u of srcs) if (!id2idx.has(u)) { id2idx.set(u, nodeIds.length); nodeIds.push(u); }
  for (const v of tgts) if (!id2idx.has(v)) { id2idx.set(v, nodeIds.length); nodeIds.push(v); }
  const edges = [];
  for (let i = 0; i < srcs.length; i++) {
    edges.push([id2idx.get(srcs[i]), id2idx.get(tgts[i])]);
  }
  return { nodeIds, edges };
}

const FIX = path.join(__dirname, "../../../tests/cd_verify/edge.csv");
const { nodeIds, edges } = loadEdgeIds(FIX);
const SEED = 1;

function run(installHooks) {
  // Clear any leftover hook from a prior run before each invocation.
  delete globalThis.__INFOMAP_DL_TERMS;
  delete globalThis.__INFOMAP_UPDATE_TERMS;
  delete globalThis.__INFOMAP_CONSOL_BEGIN;
  delete globalThis.__INFOMAP_CONSOL_PRE;
  delete globalThis.__INFOMAP_CONSOL_OP;
  delete globalThis.__INFOMAP_CONSOL_SORTED;
  let dlCount = 0, utCount = 0, csBegin = 0, csPre = 0, csOp = 0, csSorted = 0;
  if (installHooks) {
    globalThis.__INFOMAP_DL_TERMS = function () { dlCount++; };
    globalThis.__INFOMAP_UPDATE_TERMS = function () { utCount++; };
    globalThis.__INFOMAP_CONSOL_BEGIN = function () { csBegin++; };
    globalThis.__INFOMAP_CONSOL_PRE = function () { csPre++; };
    globalThis.__INFOMAP_CONSOL_OP = function () { csOp++; };
    globalThis.__INFOMAP_CONSOL_SORTED = function () { csSorted++; };
  }
  const out = COMDET.INFOMAP_CANON.runInfomapCanonical(nodeIds, edges, {
    seed: SEED, aggregationLimit: 30,
  });
  const mem = nodeIds.map(function (id) { return out.membership.get(id); });
  return {
    finalL: out.finalL,
    mem: mem,
    counts: { dlCount, utCount, csBegin, csPre, csOp, csSorted },
  };
}

const _bbuf = new Float64Array(1);
const _bview = new BigUint64Array(_bbuf.buffer);
function bits(x) { _bbuf[0] = x; return _bview[0]; }

const noHooks = run(false);
const withHooks = run(true);

function memEq(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

const lEq = bits(noHooks.finalL) === bits(withHooks.finalL);
const mEq = memEq(noHooks.mem, withHooks.mem);

console.log("=== Infomap hooks-installed-vs-uninstalled equivalence ===");
console.log(`fixture: fixture32 (n=${nodeIds.length}, m=${edges.length}), seed=${SEED}`);
console.log(`no hooks installed:  finalL=${noHooks.finalL}`);
console.log(`P0 hooks installed:  finalL=${withHooks.finalL}`);
console.log(`probe-call counts:   dl=${withHooks.counts.dlCount} ut=${withHooks.counts.utCount}`
  + ` csBegin=${withHooks.counts.csBegin} csPre=${withHooks.counts.csPre}`
  + ` csOp=${withHooks.counts.csOp} csSorted=${withHooks.counts.csSorted}`);
console.log(`finalL bit-equal:    ${lEq ? "PASS" : "FAIL"}`);
console.log(`membership equal:    ${mEq ? "PASS" : "FAIL"}`);
if (!lEq || !mEq) {
  console.error("FAIL: P0 hook installation perturbed kernel state.");
  process.exit(1);
}
console.log("PASS: P0 hooks have no kernel-state side-effect when installed.");
